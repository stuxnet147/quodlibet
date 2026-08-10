/* O_CLOEXEC is POSIX.1-2008 and the target-wide _POSIX_C_SOURCE is 200112L, so
   this translation unit asks for the later level before any header sees the
   old one. Same shape as the _GNU_SOURCE handling in src/log.c. The snapshot
   descriptors have to be close-on-exec from the moment they exist; see
   open_binary_read. */
#if !defined(_WIN32)
#  undef _POSIX_C_SOURCE
#  define _POSIX_C_SOURCE 200809L
#endif

#include "process_runner.h"

#include <limits.h>
#include <signal.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32)
#  include <share.h>
#else
#  include <fcntl.h>
#  include <sys/stat.h>
#  include <unistd.h>
#endif

#include <uv.h>

#include "blake3.h"

/* Extracted verbatim from the Bitwuzla adapter in src/solver.c. Every comment
   below that cites a measurement is describing a fix that was made there and
   is preserved here unchanged; the extraction is behaviour-preserving, and the
   only deliberate difference is that the runner takes ql_process_limits_v1
   instead of ql_solver_check_request_v1. */

/* Time allowed between child exit and pipe EOF callbacks. This is not a
   liveness bound on the child: the overall deadline still applies above it.
   250ms was measured too tight on Windows under concurrent load (4-way batch:
   6 of 60 checks lost their fully written output because the EOF callback
   lagged the exit), and a spurious transport error there costs a verdict while
   a slow EOF costs only latency on an already-failed path. */
#define QL_PROCESS_DRAIN_GRACE_MS UINT64_C(5000)
#define QL_PROCESS_PATH_CAPACITY 32768u

/* The two captured streams are still described as the solver's in every
   diagnostic. The runner's consumers are a decision procedure and the checker
   that audits its proof, so the word stays accurate, and keeping it means the
   extraction changes no message a caller or a test can observe. */
static const char QL_PROCESS_STDOUT_DESCRIPTION[] = "solver stdout";
static const char QL_PROCESS_STDERR_DESCRIPTION[] = "solver stderr";

/* The runner carries its own growable byte buffer rather than borrowing the
   one src/solver.c uses to build SMT-LIB. That buffer belongs to the query
   builder; sharing it would tie the process layer to a text format it has no
   business knowing. */
typedef struct ql_process_buffer {
    ql_allocator allocator;
    char *data;
    size_t size;
    size_t capacity;
} ql_process_buffer;

static void process_buffer_init(ql_process_buffer *buffer,
                                const ql_allocator *allocator) {
    memset(buffer, 0, sizeof(*buffer));
    buffer->allocator = *allocator;
}

static void process_buffer_dispose(ql_process_buffer *buffer) {
    if (buffer == NULL) {
        return;
    }
    if (buffer->data != NULL && ql_allocator_is_valid(&buffer->allocator)) {
        buffer->allocator.deallocate(buffer->allocator.user_data,
                                     buffer->data);
    }
    buffer->data = NULL;
    buffer->size = 0u;
    buffer->capacity = 0u;
}

static ql_status process_buffer_reserve(ql_process_buffer *buffer,
                                        size_t additional, ql_error *error) {
    size_t required;
    size_t capacity;
    void *allocation;

    if (additional > SIZE_MAX - buffer->size) {
        ql_error_set(error, QL_STATUS_OUT_OF_MEMORY,
                     "process capture buffer size overflow");
        return QL_STATUS_OUT_OF_MEMORY;
    }
    required = buffer->size + additional;
    if (required <= buffer->capacity) {
        return QL_STATUS_OK;
    }
    capacity = buffer->capacity == 0u ? 256u : buffer->capacity;
    while (capacity < required) {
        if (capacity > SIZE_MAX / 2u) {
            capacity = required;
            break;
        }
        capacity *= 2u;
    }
    allocation = buffer->allocator.reallocate(
        buffer->allocator.user_data, buffer->data, capacity);
    if (allocation == NULL) {
        ql_error_set(error, QL_STATUS_OUT_OF_MEMORY,
                     "could not grow process capture buffer to %zu bytes",
                     capacity);
        return QL_STATUS_OUT_OF_MEMORY;
    }
    buffer->data = (char *)allocation;
    buffer->capacity = capacity;
    return QL_STATUS_OK;
}

static ql_status process_buffer_append_limited(ql_process_buffer *buffer,
                                               const void *data, size_t size,
                                               uint64_t limit,
                                               const char *description,
                                               ql_error *error) {
    ql_status status;

    if ((uint64_t)buffer->size > limit ||
        (uint64_t)size > limit - (uint64_t)buffer->size) {
        ql_error_set(error, QL_STATUS_INVALID_ARGUMENT,
                     "%s exceeds the hard limit of %llu bytes", description,
                     (unsigned long long)limit);
        return QL_STATUS_INVALID_ARGUMENT;
    }
    status = process_buffer_reserve(buffer, size, error);
    if (status != QL_STATUS_OK) {
        return status;
    }
    if (size != 0u) {
        memcpy(buffer->data + buffer->size, data, size);
        buffer->size += size;
    }
    return QL_STATUS_OK;
}

/* Hands the bytes to the caller with a terminator the size does not count, so
   a result may be scanned as text without a copy. The allocation is always
   made, so an empty capture is an empty string rather than a null pointer. */
static ql_status process_buffer_release(ql_process_buffer *buffer,
                                        char **text, size_t *size,
                                        ql_error *error) {
    const ql_status status = process_buffer_reserve(buffer, 1u, error);

    if (status != QL_STATUS_OK) {
        return status;
    }
    buffer->data[buffer->size] = '\0';
    *text = buffer->data;
    *size = buffer->size;
    memset(buffer, 0, sizeof(*buffer));
    return QL_STATUS_OK;
}

typedef struct ql_process_capture ql_process_capture;

typedef struct ql_process_read_stream {
    uv_pipe_t pipe;
    ql_process_capture *capture;
    ql_process_buffer *output;
    uint64_t hard_limit;
    const char *description;
    int eof_observed;
} ql_process_read_stream;

struct ql_process_capture {
    ql_allocator allocator;
    uv_loop_t *loop;
    uv_process_t process;
    uv_pipe_t child_stdin;
    ql_process_read_stream child_stdout;
    ql_process_read_stream child_stderr;
    uv_timer_t timer;
    uv_write_t write_request;
    uv_buf_t write_buffer;
    ql_process_buffer stdout_text;
    ql_process_buffer stderr_text;
    const void *cancel_state;
    uint32_t (QL_CALL *is_cancelled)(const void *cancel_state);
    uint64_t started_ms;
    uint64_t watchdog_deadline_ms;
    int64_t exit_status;
    int term_signal;
    int process_spawned;
    int process_exited;
    int timer_initialized;
    int write_pending;
    int cancelled;
    int timed_out;
    int killed;
    int kill_escalated;
    uint64_t force_kill_deadline_ms;
    uint64_t drain_deadline_ms;
    ql_status callback_status;
    char callback_message[QL_ERROR_MESSAGE_CAPACITY];
};

void ql_process_limits_init(ql_process_limits_v1 *limits) {
    if (limits == NULL) {
        return;
    }
    memset(limits, 0, sizeof(*limits));
    limits->struct_size = sizeof(*limits);
    limits->stdout_limit_bytes = QL_PROCESS_DEFAULT_OUTPUT_LIMIT;
    limits->stderr_limit_bytes = QL_PROCESS_DEFAULT_OUTPUT_LIMIT;
}

void ql_process_result_init(ql_process_result_v1 *result) {
    if (result == NULL) {
        return;
    }
    memset(result, 0, sizeof(*result));
    result->struct_size = sizeof(*result);
}

void ql_process_result_dispose(const ql_allocator *allocator,
                               ql_process_result_v1 *result) {
    if (result == NULL) {
        return;
    }
    if (allocator != NULL && ql_allocator_is_valid(allocator)) {
        if (result->stdout_text != NULL) {
            allocator->deallocate(allocator->user_data, result->stdout_text);
        }
        if (result->stderr_text != NULL) {
            allocator->deallocate(allocator->user_data, result->stderr_text);
        }
    }
    memset(result, 0, sizeof(*result));
    result->struct_size = sizeof(*result);
}

static void close_uv_handle(uv_handle_t *handle) {
    if (handle != NULL && !uv_is_closing(handle)) {
        uv_close(handle, NULL);
    }
}

static void close_walk_cb(uv_handle_t *handle, void *unused) {
    (void)unused;
    close_uv_handle(handle);
}

/* uv_loop_close refuses with UV_EBUSY while any non-internal handle or request
   is still attached, and on that path it skips uv__loop_close, which is what
   removes this loop's SIGCHLD watcher from libuv's process-wide signal tree
   (third_party/libuv/src/uv-common.c:878). The loop lives inside a
   ql_process_capture on ql_process_run's stack, so a refused close leaves that
   global tree pointing into a frame that is about to die, and the next
   uv_spawn on any thread walks the tree and reads it. Measured: SIGSEGV in 5
   of 10 sixteen-worker batches, ASan naming stack-use-after-return in
   uv__signal_compare. docs/perf/baseline.md carries the diagnosis.

   So closing is not best-effort here. Close every handle still standing, run
   the loop until their close callbacks have fired, and only then close the
   loop. The bound exists because an unbounded loop in a teardown path is its
   own failure mode; each pass makes progress, so reaching the bound means
   something is wrong that retrying will not fix. */
#define QL_PROCESS_LOOP_DRAIN_PASSES 64u

static int drain_and_close_loop(uv_loop_t *loop) {
    unsigned pass;
    int status = uv_loop_close(loop);

    for (pass = 0u; status == UV_EBUSY && pass < QL_PROCESS_LOOP_DRAIN_PASSES;
         ++pass) {
        uv_walk(loop, close_walk_cb, NULL);
        (void)uv_run(loop, UV_RUN_DEFAULT);
        status = uv_loop_close(loop);
    }
    return status;
}

static int process_outputs_drained(const ql_process_capture *capture) {
    return capture->child_stdout.eof_observed &&
           capture->child_stderr.eof_observed;
}

static void process_close_timer(ql_process_capture *capture) {
    if (capture->timer_initialized &&
        !uv_is_closing((uv_handle_t *)&capture->timer)) {
        uv_timer_stop(&capture->timer);
        close_uv_handle((uv_handle_t *)&capture->timer);
    }
}

static void process_finish_if_drained(ql_process_capture *capture) {
    if (capture->process_exited && process_outputs_drained(capture)) {
        process_close_timer(capture);
    }
}

static void process_request_termination(ql_process_capture *capture) {
    int uv_status;

    if (!capture->process_spawned || capture->killed) {
        return;
    }
    capture->killed = 1;
    capture->force_kill_deadline_ms = uv_now(capture->loop) + 50u;
    uv_status = uv_process_kill(&capture->process, SIGTERM);
    if (uv_status != 0 && uv_status != UV_ESRCH &&
        capture->callback_status == QL_STATUS_OK) {
        capture->callback_status = QL_STATUS_IO_ERROR;
        snprintf(capture->callback_message,
                 sizeof(capture->callback_message),
                 "could not terminate solver process: %s",
                 uv_strerror(uv_status));
    }
}

static void process_set_callback_error(ql_process_capture *capture,
                                       ql_status status,
                                       const char *message) {
    if (capture->callback_status != QL_STATUS_OK) {
        return;
    }
    capture->callback_status = status;
    snprintf(capture->callback_message, sizeof(capture->callback_message),
             "%s", message);
    process_request_termination(capture);
}

static void process_alloc_cb(uv_handle_t *handle, size_t suggested_size,
                             uv_buf_t *buffer) {
    size_t allocation_size = suggested_size;
    (void)handle;

    if (allocation_size == 0u || allocation_size > 65536u) {
        allocation_size = 65536u;
    }
    buffer->base = (char *)malloc(allocation_size);
    buffer->len = buffer->base == NULL ? 0u : (unsigned int)allocation_size;
}

static void process_read_cb(uv_stream_t *stream, ssize_t nread,
                            const uv_buf_t *buffer) {
    ql_process_read_stream *read_stream =
        (ql_process_read_stream *)stream->data;
    ql_process_capture *capture = read_stream->capture;

    if (nread > 0) {
        ql_error ignored_error;
        ql_error_clear(&ignored_error);
        if (process_buffer_append_limited(
                read_stream->output, buffer->base, (size_t)nread,
                read_stream->hard_limit, read_stream->description,
                &ignored_error) != QL_STATUS_OK) {
            process_set_callback_error(capture, QL_STATUS_METHOD_ERROR,
                                       ignored_error.message);
        }
    } else if (nread < 0) {
        if (nread != UV_EOF) {
            process_set_callback_error(capture, QL_STATUS_IO_ERROR,
                                       "could not read solver process output");
        }
        read_stream->eof_observed = 1;
        close_uv_handle((uv_handle_t *)stream);
        process_finish_if_drained(capture);
    }
    free(buffer->base);
}

static void process_write_cb(uv_write_t *request, int status) {
    ql_process_capture *capture =
        (ql_process_capture *)request->data;
    capture->write_pending = 0;
    if (status < 0) {
        process_set_callback_error(capture, QL_STATUS_IO_ERROR,
                                   "could not write input to solver process");
    }
    close_uv_handle((uv_handle_t *)&capture->child_stdin);
}

static void process_exit_cb(uv_process_t *process, int64_t exit_status,
                            int term_signal) {
    ql_process_capture *capture =
        (ql_process_capture *)process->data;
    const uint64_t now = uv_now(capture->loop);

    capture->exit_status = exit_status;
    capture->term_signal = term_signal;
    capture->process_exited = 1;
    capture->drain_deadline_ms =
        now > UINT64_MAX - QL_PROCESS_DRAIN_GRACE_MS ?
            UINT64_MAX : now + QL_PROCESS_DRAIN_GRACE_MS;
    close_uv_handle((uv_handle_t *)process);
    close_uv_handle((uv_handle_t *)&capture->child_stdin);
    process_finish_if_drained(capture);
}

static void process_timer_cb(uv_timer_t *timer) {
    ql_process_capture *capture =
        (ql_process_capture *)timer->data;
    uint64_t now;

    if (!capture->process_spawned) {
        return;
    }
    now = uv_now(capture->loop);
    if (capture->process_exited) {
        if (process_outputs_drained(capture)) {
            process_close_timer(capture);
        } else if (capture->drain_deadline_ms != 0u &&
                   now >= capture->drain_deadline_ms) {
            if (capture->callback_status == QL_STATUS_OK) {
                capture->callback_status = QL_STATUS_IO_ERROR;
                snprintf(capture->callback_message,
                         sizeof(capture->callback_message),
                         "solver output pipes remained open after the child exit drain deadline");
            }
            capture->child_stdout.eof_observed = 1;
            capture->child_stderr.eof_observed = 1;
            (void)uv_read_stop(
                (uv_stream_t *)&capture->child_stdout.pipe);
            (void)uv_read_stop(
                (uv_stream_t *)&capture->child_stderr.pipe);
            close_uv_handle(
                (uv_handle_t *)&capture->child_stdout.pipe);
            close_uv_handle(
                (uv_handle_t *)&capture->child_stderr.pipe);
            process_close_timer(capture);
        }
        return;
    }
    if (capture->killed) {
        if (!capture->kill_escalated &&
            capture->force_kill_deadline_ms != 0u &&
            now >= capture->force_kill_deadline_ms) {
            const int uv_status =
                uv_process_kill(&capture->process, SIGKILL);
            capture->kill_escalated = 1;
            if (uv_status != 0 && uv_status != UV_ESRCH &&
                capture->callback_status == QL_STATUS_OK) {
                capture->callback_status = QL_STATUS_IO_ERROR;
                snprintf(capture->callback_message,
                         sizeof(capture->callback_message),
                         "could not force-kill solver process: %s",
                         uv_strerror(uv_status));
            }
        }
        return;
    }
    if (capture->is_cancelled != NULL &&
        capture->is_cancelled(capture->cancel_state) != 0u) {
        capture->cancelled = 1;
        process_request_termination(capture);
        return;
    }
    if (capture->watchdog_deadline_ms != 0u &&
        now >= capture->watchdog_deadline_ms) {
        capture->timed_out = 1;
        process_request_termination(capture);
    }
}

static uint64_t process_output_limit(size_t requested) {
    return requested == 0u ? (uint64_t)QL_PROCESS_DEFAULT_OUTPUT_LIMIT
                           : (uint64_t)requested;
}

ql_status ql_process_run(const ql_allocator *allocator,
                         const char *executable_path,
                         const char *const *arguments, size_t argument_count,
                         const void *input, size_t input_size,
                         const ql_process_limits_v1 *limits,
                         ql_process_result_v1 *result, ql_error *error) {
    static const size_t minimum_limits_size =
        offsetof(ql_process_limits_v1, reserved);
    ql_process_capture capture;
    uv_process_options_t options;
    uv_stdio_container_t stdio[3];
    char **argv = NULL;
    size_t index;
    int uv_status;
    ql_status status = QL_STATUS_OK;

    if (allocator == NULL || !ql_allocator_is_valid(allocator) ||
        executable_path == NULL || executable_path[0] == '\0' ||
        result == NULL || (argument_count != 0u && arguments == NULL) ||
        (input_size != 0u && input == NULL)) {
        ql_error_set(error, QL_STATUS_INVALID_ARGUMENT,
                     "process run requires an allocator, an executable path, and a result");
        return QL_STATUS_INVALID_ARGUMENT;
    }
    if (limits != NULL && limits->struct_size < minimum_limits_size) {
        ql_error_set(error, QL_STATUS_ABI_MISMATCH,
                     "process limits structure is smaller than this build understands");
        return QL_STATUS_ABI_MISMATCH;
    }
    if (input_size > UINT_MAX) {
        ql_error_set(error, QL_STATUS_INVALID_ARGUMENT,
                     "process input exceeds the transport limit");
        return QL_STATUS_INVALID_ARGUMENT;
    }
    if (argument_count > SIZE_MAX / sizeof(char *) - 2u) {
        ql_error_set(error, QL_STATUS_INVALID_ARGUMENT,
                     "process argument vector is too large");
        return QL_STATUS_INVALID_ARGUMENT;
    }

    ql_process_result_init(result);
    memset(&capture, 0, sizeof(capture));
    capture.allocator = *allocator;
    capture.callback_status = QL_STATUS_OK;
    if (limits != NULL) {
        capture.cancel_state = limits->cancel_state;
        capture.is_cancelled = limits->is_cancelled;
    }
    process_buffer_init(&capture.stdout_text, allocator);
    process_buffer_init(&capture.stderr_text, allocator);

    /* argv[0] is the path the runner spawned, never something the caller
       supplied separately: a caller cannot then disagree with what ran. */
    argv = (char **)allocator->allocate(allocator->user_data,
                                        (argument_count + 2u) *
                                            sizeof(char *));
    if (argv == NULL) {
        ql_error_set(error, QL_STATUS_OUT_OF_MEMORY,
                     "could not allocate the process argument vector");
        status = QL_STATUS_OUT_OF_MEMORY;
        goto fail_without_loop;
    }
    argv[0] = (char *)(uintptr_t)executable_path;
    for (index = 0u; index < argument_count; ++index) {
        if (arguments[index] == NULL) {
            allocator->deallocate(allocator->user_data, argv);
            argv = NULL;
            ql_error_set(error, QL_STATUS_INVALID_ARGUMENT,
                         "process argument %zu is null", index);
            status = QL_STATUS_INVALID_ARGUMENT;
            goto fail_without_loop;
        }
        argv[index + 1u] = (char *)(uintptr_t)arguments[index];
    }
    argv[argument_count + 1u] = NULL;

    /* The loop is allocated rather than held in this frame because libuv
       registers it in a process-wide structure that outlives a refused close.
       See drain_and_close_loop: on the path where the loop cannot be closed
       the allocation is deliberately not freed, which costs a bounded leak and
       buys the guarantee that nothing global ever points at dead memory. */
    capture.loop = allocator->allocate(allocator->user_data,
                                       sizeof(*capture.loop));
    if (capture.loop == NULL) {
        ql_error_set(error, QL_STATUS_OUT_OF_MEMORY,
                     "could not allocate solver process loop");
        status = QL_STATUS_OUT_OF_MEMORY;
        goto fail_without_loop;
    }
    memset(capture.loop, 0, sizeof(*capture.loop));

    uv_status = uv_loop_init(capture.loop);
    if (uv_status != 0) {
        allocator->deallocate(allocator->user_data, capture.loop);
        capture.loop = NULL;
        ql_error_set(error, QL_STATUS_IO_ERROR,
                     "could not initialize solver process loop: %s",
                     uv_strerror(uv_status));
        status = QL_STATUS_IO_ERROR;
        goto fail_without_loop;
    }
    uv_update_time(capture.loop);
    capture.started_ms = uv_now(capture.loop);
    if (limits != NULL && limits->timeout_ms != 0u) {
        uint64_t deadline = capture.started_ms;
        if (limits->timeout_ms >
            UINT64_MAX - deadline - QL_PROCESS_WATCHDOG_GRACE_MS) {
            deadline = UINT64_MAX;
        } else {
            deadline += limits->timeout_ms + QL_PROCESS_WATCHDOG_GRACE_MS;
        }
        capture.watchdog_deadline_ms = deadline;
    }

    uv_status = uv_pipe_init(capture.loop, &capture.child_stdin, 0);
    if (uv_status != 0) {
        ql_error_set(error, QL_STATUS_IO_ERROR,
                     "could not initialize solver stdin pipe: %s",
                     uv_strerror(uv_status));
        status = QL_STATUS_IO_ERROR;
        goto close_loop;
    }
    uv_status = uv_pipe_init(capture.loop, &capture.child_stdout.pipe, 0);
    if (uv_status != 0) {
        ql_error_set(error, QL_STATUS_IO_ERROR,
                     "could not initialize solver stdout pipe: %s",
                     uv_strerror(uv_status));
        status = QL_STATUS_IO_ERROR;
        close_uv_handle((uv_handle_t *)&capture.child_stdin);
        goto drain_loop;
    }
    uv_status = uv_pipe_init(capture.loop, &capture.child_stderr.pipe, 0);
    if (uv_status != 0) {
        ql_error_set(error, QL_STATUS_IO_ERROR,
                     "could not initialize solver stderr pipe: %s",
                     uv_strerror(uv_status));
        status = QL_STATUS_IO_ERROR;
        close_uv_handle((uv_handle_t *)&capture.child_stdin);
        close_uv_handle((uv_handle_t *)&capture.child_stdout.pipe);
        goto drain_loop;
    }
    capture.child_stdout.capture = &capture;
    capture.child_stdout.output = &capture.stdout_text;
    capture.child_stdout.hard_limit = process_output_limit(
        limits == NULL ? 0u : limits->stdout_limit_bytes);
    capture.child_stdout.description = QL_PROCESS_STDOUT_DESCRIPTION;
    capture.child_stdout.pipe.data = &capture.child_stdout;
    capture.child_stderr.capture = &capture;
    capture.child_stderr.output = &capture.stderr_text;
    capture.child_stderr.hard_limit = process_output_limit(
        limits == NULL ? 0u : limits->stderr_limit_bytes);
    capture.child_stderr.description = QL_PROCESS_STDERR_DESCRIPTION;
    capture.child_stderr.pipe.data = &capture.child_stderr;

    memset(stdio, 0, sizeof(stdio));
    stdio[0].flags = UV_CREATE_PIPE | UV_READABLE_PIPE;
    stdio[0].data.stream = (uv_stream_t *)&capture.child_stdin;
    stdio[1].flags = UV_CREATE_PIPE | UV_WRITABLE_PIPE;
    stdio[1].data.stream = (uv_stream_t *)&capture.child_stdout.pipe;
    stdio[2].flags = UV_CREATE_PIPE | UV_WRITABLE_PIPE;
    stdio[2].data.stream = (uv_stream_t *)&capture.child_stderr.pipe;

    memset(&options, 0, sizeof(options));
    options.exit_cb = process_exit_cb;
    options.file = executable_path;
    options.args = argv;
    options.stdio_count = 3;
    options.stdio = stdio;
#if defined(_WIN32)
    options.flags = UV_PROCESS_WINDOWS_HIDE;
#endif
    capture.process.data = &capture;
    uv_status = uv_spawn(capture.loop, &capture.process, &options);
    if (uv_status != 0) {
        ql_error_set(error, QL_STATUS_NOT_FOUND,
                     "could not start solver executable '%s': %s",
                     executable_path, uv_strerror(uv_status));
        status = QL_STATUS_NOT_FOUND;
        close_uv_handle((uv_handle_t *)&capture.child_stdin);
        close_uv_handle((uv_handle_t *)&capture.child_stdout.pipe);
        close_uv_handle((uv_handle_t *)&capture.child_stderr.pipe);
        goto drain_loop;
    }
    capture.process_spawned = 1;

    uv_status = uv_read_start((uv_stream_t *)&capture.child_stdout.pipe,
                              process_alloc_cb, process_read_cb);
    if (uv_status == 0) {
        uv_status = uv_read_start((uv_stream_t *)&capture.child_stderr.pipe,
                                  process_alloc_cb, process_read_cb);
    }
    if (uv_status != 0) {
        process_set_callback_error(&capture, QL_STATUS_IO_ERROR,
                                   "could not start solver output capture");
        capture.child_stdout.eof_observed = 1;
        capture.child_stderr.eof_observed = 1;
        close_uv_handle((uv_handle_t *)&capture.child_stdout.pipe);
        close_uv_handle((uv_handle_t *)&capture.child_stderr.pipe);
    }

    uv_status = uv_timer_init(capture.loop, &capture.timer);
    if (uv_status == 0) {
        capture.timer_initialized = 1;
        capture.timer.data = &capture;
        uv_status = uv_timer_start(&capture.timer, process_timer_cb, 1u, 5u);
    }
    if (uv_status != 0) {
        int kill_status;
        process_set_callback_error(&capture, QL_STATUS_IO_ERROR,
                                   "could not start solver process watchdog");
        kill_status = uv_process_kill(&capture.process, SIGKILL);
        capture.kill_escalated = 1;
        if (kill_status != 0 && kill_status != UV_ESRCH) {
            snprintf(capture.callback_message,
                     sizeof(capture.callback_message),
                     "solver watchdog failed and force-kill failed: %s",
                     uv_strerror(kill_status));
        }
        capture.child_stdout.eof_observed = 1;
        capture.child_stderr.eof_observed = 1;
        (void)uv_read_stop((uv_stream_t *)&capture.child_stdout.pipe);
        (void)uv_read_stop((uv_stream_t *)&capture.child_stderr.pipe);
        close_uv_handle((uv_handle_t *)&capture.child_stdout.pipe);
        close_uv_handle((uv_handle_t *)&capture.child_stderr.pipe);
        process_close_timer(&capture);
    }

    if (input_size == 0u) {
        close_uv_handle((uv_handle_t *)&capture.child_stdin);
    } else {
        capture.write_buffer = uv_buf_init((char *)(uintptr_t)input,
                                           (unsigned int)input_size);
        capture.write_request.data = &capture;
        capture.write_pending = 1;
        uv_status = uv_write(&capture.write_request,
                             (uv_stream_t *)&capture.child_stdin,
                             &capture.write_buffer, 1u, process_write_cb);
        if (uv_status != 0) {
            capture.write_pending = 0;
            process_set_callback_error(&capture, QL_STATUS_IO_ERROR,
                                       "could not submit input to solver process");
            close_uv_handle((uv_handle_t *)&capture.child_stdin);
        }
    }

    (void)uv_run(capture.loop, UV_RUN_DEFAULT);
    if (capture.callback_status != QL_STATUS_OK) {
        ql_error_set(error, capture.callback_status, "%s",
                     capture.callback_message);
        status = capture.callback_status;
        goto close_loop;
    }

    status = process_buffer_release(&capture.stdout_text,
                                    &result->stdout_text,
                                    &result->stdout_size, error);
    if (status == QL_STATUS_OK) {
        status = process_buffer_release(&capture.stderr_text,
                                        &result->stderr_text,
                                        &result->stderr_size, error);
    }
    if (status == QL_STATUS_OK) {
        result->exit_status = capture.exit_status;
        result->term_signal = capture.term_signal;
        result->cancelled = (uint32_t)capture.cancelled;
        result->timed_out = (uint32_t)capture.timed_out;
        result->killed = (uint32_t)capture.killed;
        ql_error_clear(error);
    }

close_loop:
    uv_status = drain_and_close_loop(capture.loop);
    if (uv_status == 0) {
        allocator->deallocate(allocator->user_data, capture.loop);
    } else {
        /* Leak it on purpose. libuv still has this loop in its process-wide
           signal tree, so freeing here would leave that tree pointing at
           reusable memory and the next uv_spawn on any thread would read it.
           One leaked loop per occurrence is a price worth paying to keep that
           impossible, and the caller still learns it happened. Not reachable
           in practice: every drain pass closes handles that cannot reopen. */
        ql_error_set(error, QL_STATUS_INTERNAL_ERROR,
                     "solver process loop retained active handles: %s",
                     uv_strerror(uv_status));
        status = QL_STATUS_INTERNAL_ERROR;
    }
    capture.loop = NULL;
    process_buffer_dispose(&capture.stdout_text);
    process_buffer_dispose(&capture.stderr_text);
    allocator->deallocate(allocator->user_data, argv);
    if (status != QL_STATUS_OK) {
        ql_process_result_dispose(allocator, result);
    }
    return status;

drain_loop:
    (void)uv_run(capture.loop, UV_RUN_DEFAULT);
    goto close_loop;

fail_without_loop:
    process_buffer_dispose(&capture.stdout_text);
    process_buffer_dispose(&capture.stderr_text);
    if (argv != NULL) {
        allocator->deallocate(allocator->user_data, argv);
    }
    ql_process_result_dispose(allocator, result);
    return status;
}

/* Every descriptor this file opens must be closed on exec.
   uv_spawn forks and execs from whichever thread is judging, and a fork
   snapshots the whole descriptor table, so a descriptor another thread happens
   to have open at that instant is inherited by that child. For a descriptor
   onto a snapshot still being written that is not untidiness, it is an
   execution failure: Linux refuses to exec a file that any process holds open
   for writing, and the sibling judgement dies with ETXTBSY. Measured at one or
   two failures per forty-eight judgements at sixteen workers.

   Requesting close-on-exec at open time is the only race-free way to do this;
   setting it afterwards leaves a window in which a fork can still copy the
   descriptor. Children need nothing from us but their three stdio streams,
   which libuv installs itself. */
static FILE *open_binary_read(const char *path) {
#if defined(_WIN32)
    /* fopen_s opens with exclusive (non-shared) access, so a concurrent
       reader -- an on-access antivirus scan of a freshly written snapshot is
       the measured case -- makes the open fail with a sharing violation.
       Hashing only reads; deny nothing. `N` keeps the handle out of children. */
    return _fsopen(path, "rbN", _SH_DENYNO);
#else
    /* glibc and the BSDs read `e` as O_CLOEXEC. Falling back to a plain open
       would silently restore the race, so ask open(2) directly instead. */
    int descriptor = open(path, O_RDONLY | O_CLOEXEC);
    FILE *file;

    if (descriptor < 0) {
        return NULL;
    }
    file = fdopen(descriptor, "rb");
    if (file == NULL) {
        (void)close(descriptor);
    }
    return file;
#endif
}

static FILE *open_binary_write(const char *path) {
#if defined(_WIN32)
    FILE *file = NULL;
    return fopen_s(&file, path, "wbN") == 0 ? file : NULL;
#else
    /* 0700: the snapshot is this process's private copy and is tightened
       further once it is written and hashed. */
    int descriptor = open(path, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC,
                          S_IRWXU);
    FILE *stream;

    if (descriptor < 0) {
        return NULL;
    }
    stream = fdopen(descriptor, "wb");
    if (stream == NULL) {
        (void)close(descriptor);
    }
    return stream;
#endif
}

int ql_process_path_is_readable(const char *path) {
    FILE *file;

    if (path == NULL || path[0] == '\0') {
        return 0;
    }
    file = open_binary_read(path);
    if (file == NULL) {
        return 0;
    }
    fclose(file);
    return 1;
}

static void cleanup_snapshot_paths(const char *executable,
                                   const char *directory) {
    uv_fs_t request;

#if defined(_WIN32)
    if (executable != NULL) {
        (void)uv_fs_chmod(NULL, &request, executable, 0666, NULL);
        uv_fs_req_cleanup(&request);
    }
#else
    if (directory != NULL) {
        (void)chmod(directory, S_IRWXU);
    }
    if (executable != NULL) {
        (void)chmod(executable, S_IRWXU);
    }
#endif
    if (executable != NULL) {
        (void)uv_fs_unlink(NULL, &request, executable, NULL);
        uv_fs_req_cleanup(&request);
    }
    if (directory != NULL) {
        (void)uv_fs_rmdir(NULL, &request, directory, NULL);
        uv_fs_req_cleanup(&request);
    }
}

static char *copy_bytes_as_cstr(const ql_allocator *allocator,
                                const char *bytes, size_t size) {
    char *copy;

    if (bytes == NULL || size == SIZE_MAX) {
        return NULL;
    }
    copy = (char *)allocator->allocate(allocator->user_data, size + 1u);
    if (copy == NULL) {
        return NULL;
    }
    memcpy(copy, bytes, size);
    copy[size] = '\0';
    return copy;
}

static ql_status copy_snapshot_bytes(const char *source_path,
                                     const char *snapshot_path,
                                     ql_error *error) {
    unsigned char bytes[65536];
    FILE *source = open_binary_read(source_path);
    FILE *snapshot = NULL;
    size_t count;
    ql_status status = QL_STATUS_OK;

    if (source == NULL) {
        ql_error_set(error, QL_STATUS_IO_ERROR,
                     "could not open selected executable for snapshotting");
        return QL_STATUS_IO_ERROR;
    }
    snapshot = open_binary_write(snapshot_path);
    if (snapshot == NULL) {
        fclose(source);
        ql_error_set(error, QL_STATUS_IO_ERROR,
                     "could not create private executable snapshot");
        return QL_STATUS_IO_ERROR;
    }
    while ((count = fread(bytes, 1u, sizeof(bytes), source)) != 0u) {
        if (fwrite(bytes, 1u, count, snapshot) != count) {
            ql_error_set(error, QL_STATUS_IO_ERROR,
                         "could not write private executable snapshot");
            status = QL_STATUS_IO_ERROR;
            break;
        }
    }
    if (status == QL_STATUS_OK && ferror(source)) {
        ql_error_set(error, QL_STATUS_IO_ERROR,
                     "could not read selected executable while snapshotting");
        status = QL_STATUS_IO_ERROR;
    }
    if (fclose(source) != 0 && status == QL_STATUS_OK) {
        ql_error_set(error, QL_STATUS_IO_ERROR,
                     "could not close selected executable after snapshotting");
        status = QL_STATUS_IO_ERROR;
    }
    if (fclose(snapshot) != 0 && status == QL_STATUS_OK) {
        ql_error_set(error, QL_STATUS_IO_ERROR,
                     "could not finalize private executable snapshot");
        status = QL_STATUS_IO_ERROR;
    }
    return status;
}

/* The temporary directory is named after the tool, not after the file: the
   name a caller asks for carries a platform extension and a directory called
   quodlibet-bitwuzla.exe-1234 reads like a mistake. Everything up to the last
   dot is the stem, which is exactly the name on platforms with no extension.
   src/solver.c's snapshot directories are still quodlibet-bitwuzla-<pid>-*. */
static ql_status snapshot_directory_stem(const char *name, char *stem,
                                         size_t capacity, ql_error *error) {
    const char *extension;
    size_t length;
    size_t index;
    int count;

    if (name == NULL || name[0] == '\0') {
        ql_error_set(error, QL_STATUS_INVALID_ARGUMENT,
                     "an executable snapshot requires a base name");
        return QL_STATUS_INVALID_ARGUMENT;
    }
    length = strlen(name);
    for (index = 0u; index < length; ++index) {
        if (name[index] == '/' || name[index] == '\\' ||
            name[index] == ':') {
            ql_error_set(error, QL_STATUS_INVALID_ARGUMENT,
                         "an executable snapshot name must not contain a path separator");
            return QL_STATUS_INVALID_ARGUMENT;
        }
    }
    extension = strrchr(name, '.');
    if (extension != NULL && extension != name) {
        length = (size_t)(extension - name);
    }
    count = snprintf(stem, capacity, "quodlibet-%.*s-%lld-XXXXXX",
                     (int)length, name, (long long)uv_os_getpid());
    if (count <= 0 || (size_t)count >= capacity) {
        ql_error_set(error, QL_STATUS_INTERNAL_ERROR,
                     "could not format the private snapshot identity");
        return QL_STATUS_INTERNAL_ERROR;
    }
    ql_error_clear(error);
    return QL_STATUS_OK;
}

ql_status ql_process_snapshot_create(const ql_allocator *allocator,
                                     const char *source_executable,
                                     const char *name,
                                     ql_process_snapshot *snapshot,
                                     ql_error *error) {
    char directory_stem[128];
    char temporary_directory[QL_PROCESS_PATH_CAPACITY];
    char directory_template[QL_PROCESS_PATH_CAPACITY];
    char snapshot_path[QL_PROCESS_PATH_CAPACITY];
    size_t temporary_size = sizeof(temporary_directory);
    char *directory = NULL;
    char *executable = NULL;
    uv_fs_t request;
    int uv_status;
    int count;
    ql_status status;

    if (allocator == NULL || !ql_allocator_is_valid(allocator) ||
        source_executable == NULL || source_executable[0] == '\0' ||
        snapshot == NULL) {
        ql_error_set(error, QL_STATUS_INVALID_ARGUMENT,
                     "an executable snapshot requires an allocator, a source path, and a destination");
        return QL_STATUS_INVALID_ARGUMENT;
    }
    memset(snapshot, 0, sizeof(*snapshot));
    status = snapshot_directory_stem(name, directory_stem,
                                     sizeof(directory_stem), error);
    if (status != QL_STATUS_OK) {
        return status;
    }
    uv_status = uv_os_tmpdir(temporary_directory, &temporary_size);
    if (uv_status != 0 || temporary_size == 0u ||
        temporary_size >= sizeof(temporary_directory)) {
        ql_error_set(error, QL_STATUS_IO_ERROR,
                     "could not locate a directory for the private executable snapshot");
        return QL_STATUS_IO_ERROR;
    }
    temporary_directory[temporary_size] = '\0';
    count = snprintf(
        directory_template, sizeof(directory_template), "%s%s%s",
        temporary_directory,
        temporary_directory[temporary_size - 1u] == '/' ||
                temporary_directory[temporary_size - 1u] == '\\' ?
            "" : "/",
        directory_stem);
    if (count <= 0 || (size_t)count >= sizeof(directory_template)) {
        ql_error_set(error, QL_STATUS_INVALID_ARGUMENT,
                     "private executable snapshot path is too long");
        return QL_STATUS_INVALID_ARGUMENT;
    }
    uv_status = uv_fs_mkdtemp(NULL, &request, directory_template, NULL);
    if (uv_status < 0) {
        ql_error_set(error, QL_STATUS_IO_ERROR,
                     "could not create private executable snapshot directory: %s",
                     uv_strerror(uv_status));
        uv_fs_req_cleanup(&request);
        return QL_STATUS_IO_ERROR;
    }
    if (strlen(request.path) >= sizeof(directory_template)) {
        (void)remove(request.path);
        uv_fs_req_cleanup(&request);
        ql_error_set(error, QL_STATUS_INVALID_ARGUMENT,
                     "private executable snapshot directory path is too long");
        return QL_STATUS_INVALID_ARGUMENT;
    }
    memcpy(directory_template, request.path, strlen(request.path) + 1u);
    directory = copy_bytes_as_cstr(allocator, directory_template,
                                   strlen(directory_template));
    uv_fs_req_cleanup(&request);
    if (directory == NULL) {
        cleanup_snapshot_paths(NULL, directory_template);
        ql_error_set(error, QL_STATUS_OUT_OF_MEMORY,
                     "could not retain private executable snapshot directory");
        return QL_STATUS_OUT_OF_MEMORY;
    }
#if !defined(_WIN32)
    if (chmod(directory, S_IRWXU) != 0) {
        cleanup_snapshot_paths(NULL, directory);
        allocator->deallocate(allocator->user_data, directory);
        ql_error_set(error, QL_STATUS_IO_ERROR,
                     "could not make the executable snapshot directory private");
        return QL_STATUS_IO_ERROR;
    }
#endif
    count = snprintf(snapshot_path, sizeof(snapshot_path), "%s/%s",
                     directory, name);
    if (count <= 0 || (size_t)count >= sizeof(snapshot_path)) {
        cleanup_snapshot_paths(NULL, directory);
        allocator->deallocate(allocator->user_data, directory);
        ql_error_set(error, QL_STATUS_INVALID_ARGUMENT,
                     "private executable path is too long");
        return QL_STATUS_INVALID_ARGUMENT;
    }
    executable = copy_bytes_as_cstr(allocator, snapshot_path, (size_t)count);
    if (executable == NULL) {
        cleanup_snapshot_paths(NULL, directory);
        allocator->deallocate(allocator->user_data, directory);
        ql_error_set(error, QL_STATUS_OUT_OF_MEMORY,
                     "could not retain private executable path");
        return QL_STATUS_OUT_OF_MEMORY;
    }
    status = copy_snapshot_bytes(source_executable, executable, error);
#if defined(_WIN32)
    if (status == QL_STATUS_OK) {
        const int chmod_status =
            uv_fs_chmod(NULL, &request, executable, 0444, NULL);
        uv_fs_req_cleanup(&request);
        if (chmod_status < 0) {
            ql_error_set(error, QL_STATUS_IO_ERROR,
                         "could not mark the private executable snapshot read-only: %s",
                         uv_strerror(chmod_status));
            status = QL_STATUS_IO_ERROR;
        }
    }
#else
    if (status == QL_STATUS_OK && chmod(executable, S_IRUSR | S_IXUSR) != 0) {
        ql_error_set(error, QL_STATUS_IO_ERROR,
                     "could not make the private executable snapshot executable");
        status = QL_STATUS_IO_ERROR;
    }
    if (status == QL_STATUS_OK &&
        chmod(directory, S_IRUSR | S_IXUSR) != 0) {
        ql_error_set(error, QL_STATUS_IO_ERROR,
                     "could not make the executable snapshot directory read-only");
        status = QL_STATUS_IO_ERROR;
    }
#endif
    if (status == QL_STATUS_OK) {
        status = ql_process_executable_digest(executable, &snapshot->digest,
                                              error);
    }
    if (status != QL_STATUS_OK) {
        cleanup_snapshot_paths(executable, directory);
        allocator->deallocate(allocator->user_data, executable);
        allocator->deallocate(allocator->user_data, directory);
        memset(snapshot, 0, sizeof(*snapshot));
        return status;
    }
    snapshot->allocator = *allocator;
    snapshot->directory = directory;
    snapshot->executable_path = executable;
    ql_error_clear(error);
    return QL_STATUS_OK;
}

void ql_process_snapshot_dispose(ql_process_snapshot *snapshot) {
    if (snapshot == NULL) {
        return;
    }
    cleanup_snapshot_paths(snapshot->executable_path, snapshot->directory);
    if (ql_allocator_is_valid(&snapshot->allocator)) {
        snapshot->allocator.deallocate(snapshot->allocator.user_data,
                                       snapshot->executable_path);
        snapshot->allocator.deallocate(snapshot->allocator.user_data,
                                       snapshot->directory);
    }
    memset(snapshot, 0, sizeof(*snapshot));
}

/* A snapshot written moments ago can still be unopenable: on Windows an
   on-access antivirus scan holds the file, and the measured window under load
   is seconds rather than milliseconds. On an 8-way concurrent batch, 11 of 320
   checks failed at 200ms of retry and 5 of 320 still failed with shared-read
   opens. Three seconds of bounded retry costs nothing on the uncontended path;
   a path that stays unopenable still fails. */
#define QL_DIGEST_OPEN_RETRIES 60u
#define QL_DIGEST_OPEN_BACKOFF_MS 50u

static FILE *open_binary_read_retry(const char *path) {
    unsigned attempt;
    FILE *file = open_binary_read(path);

    for (attempt = 0u; file == NULL && attempt < QL_DIGEST_OPEN_RETRIES;
         ++attempt) {
        uv_sleep(QL_DIGEST_OPEN_BACKOFF_MS);
        file = open_binary_read(path);
    }
    return file;
}

ql_status ql_process_executable_digest(const char *path, ql_digest *digest,
                                       ql_error *error) {
    unsigned char bytes[65536];
    blake3_hasher hasher;
    FILE *file;
    size_t count;

    if (path == NULL || path[0] == '\0' || digest == NULL) {
        ql_error_set(error, QL_STATUS_INVALID_ARGUMENT,
                     "hashing an executable requires a path and a digest");
        return QL_STATUS_INVALID_ARGUMENT;
    }
    file = open_binary_read_retry(path);
    if (file == NULL) {
        ql_error_set(error, QL_STATUS_IO_ERROR,
                     "could not open executable for identity hashing");
        return QL_STATUS_IO_ERROR;
    }
    blake3_hasher_init(&hasher);
    while ((count = fread(bytes, 1u, sizeof(bytes), file)) != 0u) {
        blake3_hasher_update(&hasher, bytes, count);
    }
    if (ferror(file)) {
        fclose(file);
        ql_error_set(error, QL_STATUS_IO_ERROR,
                     "could not read executable for identity hashing");
        return QL_STATUS_IO_ERROR;
    }
    if (fclose(file) != 0) {
        ql_error_set(error, QL_STATUS_IO_ERROR,
                     "could not close executable after identity hashing");
        return QL_STATUS_IO_ERROR;
    }
    blake3_hasher_finalize(&hasher, digest->bytes, QL_DIGEST_SIZE);
    ql_error_clear(error);
    return QL_STATUS_OK;
}
