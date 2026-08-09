/* O_CLOEXEC is POSIX.1-2008 and the target-wide _POSIX_C_SOURCE is 200112L, so
   this translation unit asks for the later level before any header sees the
   old one. Same shape as the _GNU_SOURCE handling in src/log.c. The snapshot
   descriptors have to be close-on-exec from the moment they exist; see
   open_binary_read. */
#if !defined(_WIN32)
#  undef _POSIX_C_SOURCE
#  define _POSIX_C_SOURCE 200809L
#endif

#include "quodlibet/solver.h"

#include <ctype.h>
#include <limits.h>
#include <signal.h>
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
#include "yyjson.h"

#ifndef QL_BITWUZLA_EXECUTABLE
#  define QL_BITWUZLA_EXECUTABLE ""
#  define QL_BITWUZLA_AVAILABILITY QL_SOLVER_UNAVAILABLE
#else
#  define QL_BITWUZLA_AVAILABILITY QL_SOLVER_AVAILABLE
#endif

#define QL_BITWUZLA_VERSION "0.9.1"
#define QL_BITWUZLA_VERSION_TIMEOUT_MS UINT64_C(5000)
#define QL_PROCESS_WATCHDOG_GRACE_MS UINT64_C(50)
/* Time allowed between child exit and pipe EOF callbacks. This is not a
   liveness bound on the solver: the overall check deadline still applies
   above it. 250ms was measured too tight on Windows under concurrent load
   (4-way batch: 6 of 60 checks lost their fully written output because the
   EOF callback lagged the exit), and a spurious transport error there costs
   a verdict while a slow EOF costs only latency on an already-failed path. */
#define QL_PROCESS_DRAIN_GRACE_MS UINT64_C(5000)
#define QL_SOLVER_PATH_CAPACITY 32768u

typedef struct ql_buffer {
    ql_allocator allocator;
    char *data;
    size_t size;
    size_t capacity;
} ql_buffer;

struct ql_smt2_builder {
    ql_buffer text;
    ql_solver_logic logic;
};

struct ql_solver {
    ql_allocator allocator;
    const ql_solver_descriptor_v1 *descriptor;
    void *backend_state;
    uint64_t stack_depth;
};

static const ql_allocator *resolve_allocator(
    const ql_allocator *allocator) {
    return allocator == NULL ? ql_default_allocator() : allocator;
}

static void buffer_init(ql_buffer *buffer, const ql_allocator *allocator) {
    memset(buffer, 0, sizeof(*buffer));
    buffer->allocator = *allocator;
}

static void buffer_dispose(ql_buffer *buffer) {
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

static ql_status buffer_reserve(ql_buffer *buffer, size_t additional,
                                ql_error *error) {
    size_t required;
    size_t capacity;
    void *allocation;

    if (additional > SIZE_MAX - buffer->size) {
        ql_error_set(error, QL_STATUS_OUT_OF_MEMORY,
                     "solver buffer size overflow");
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
                     "could not grow solver buffer to %zu bytes", capacity);
        return QL_STATUS_OUT_OF_MEMORY;
    }
    buffer->data = (char *)allocation;
    buffer->capacity = capacity;
    return QL_STATUS_OK;
}

static ql_status buffer_append(ql_buffer *buffer, const void *data,
                               size_t size, ql_error *error) {
    ql_status status;

    if (size != 0u && data == NULL) {
        ql_error_set(error, QL_STATUS_INVALID_ARGUMENT,
                     "non-empty solver buffer append requires data");
        return QL_STATUS_INVALID_ARGUMENT;
    }
    status = buffer_reserve(buffer, size, error);
    if (status != QL_STATUS_OK) {
        return status;
    }
    if (size != 0u) {
        memcpy(buffer->data + buffer->size, data, size);
        buffer->size += size;
    }
    return QL_STATUS_OK;
}

static ql_status buffer_append_limited(ql_buffer *buffer, const void *data,
                                       size_t size, uint64_t limit,
                                       const char *description,
                                       ql_error *error) {
    if ((uint64_t)buffer->size > limit || (uint64_t)size > limit -
            (uint64_t)buffer->size) {
        ql_error_set(error, QL_STATUS_INVALID_ARGUMENT,
                     "%s exceeds the hard limit of %llu bytes", description,
                     (unsigned long long)limit);
        return QL_STATUS_INVALID_ARGUMENT;
    }
    return buffer_append(buffer, data, size, error);
}

static ql_status buffer_append_cstr(ql_buffer *buffer, const char *text,
                                    ql_error *error) {
    return buffer_append(buffer, text, strlen(text), error);
}

static int logic_is_single(ql_solver_logic logic) {
    return logic != 0u && (logic & (logic - 1u)) == 0u &&
           (logic & ~QL_SOLVER_LOGIC_ALL) == 0u;
}

static const char *logic_name(ql_solver_logic logic) {
    switch (logic) {
    case QL_SOLVER_LOGIC_QF_BV:
        return "QF_BV";
    case QL_SOLVER_LOGIC_QF_ABV:
        return "QF_ABV";
    case QL_SOLVER_LOGIC_QF_FP:
        return "QF_FP";
    case QL_SOLVER_LOGIC_QF_BVFP:
        return "QF_BVFP";
    case QL_SOLVER_LOGIC_QF_AUFBV:
        return "QF_AUFBV";
    default:
        return NULL;
    }
}

static int logic_uses_bv(ql_solver_logic logic) {
    return (logic & (QL_SOLVER_LOGIC_QF_BV | QL_SOLVER_LOGIC_QF_ABV |
                     QL_SOLVER_LOGIC_QF_BVFP |
                     QL_SOLVER_LOGIC_QF_AUFBV)) != 0u;
}

static int valid_symbol(const char *symbol) {
    const unsigned char *cursor = (const unsigned char *)symbol;

    if (cursor == NULL || *cursor == '\0' || isdigit(*cursor)) {
        return 0;
    }
    for (; *cursor != '\0'; ++cursor) {
        if (!(isalnum(*cursor) || strchr("~!@$%^&*_-+=<>.?/", *cursor) !=
                                      NULL)) {
            return 0;
        }
    }
    return 1;
}

static int valid_single_line_term(const char *term) {
    const unsigned char *cursor = (const unsigned char *)term;

    if (cursor == NULL || *cursor == '\0') {
        return 0;
    }
    for (; *cursor != '\0'; ++cursor) {
        if (*cursor == '\r' || *cursor == '\n' || *cursor == '\0') {
            return 0;
        }
    }
    return 1;
}

static int token_equals(const char *token, size_t size,
                        const char *expected) {
    const size_t expected_size = strlen(expected);
    return size == expected_size && memcmp(token, expected, size) == 0;
}

static int allowed_top_level_command(const char *token, size_t size) {
    static const char *const allowed[] = {
        "set-logic",       "declare-sort",    "define-sort",
        "declare-const",   "declare-fun",     "define-const",
        "define-fun",      "define-fun-rec",  "define-funs-rec",
        "assert",
    };
    size_t index;

    for (index = 0u; index < sizeof(allowed) / sizeof(allowed[0]); ++index) {
        if (token_equals(token, size, allowed[index])) {
            return 1;
        }
    }
    return 0;
}

static void skip_space_and_comments(const char *text, size_t size,
                                    size_t *offset) {
    size_t cursor = *offset;

    for (;;) {
        while (cursor < size && isspace((unsigned char)text[cursor])) {
            ++cursor;
        }
        if (cursor >= size || text[cursor] != ';') {
            break;
        }
        while (cursor < size && text[cursor] != '\n') {
            ++cursor;
        }
    }
    *offset = cursor;
}

/* This is a command-boundary whitelist, not an SMT parser. Its purpose is to
   keep check-sat, reset, push/pop, and output commands out of a transcript
   whose terminal query is controlled by Quodlibet. Bitwuzla remains the only
   component that parses and solves SMT-LIB. */
static ql_status validate_smt2_batch(const char *text, size_t size,
                                     ql_error *error) {
    size_t cursor = 0u;

    if (text == NULL || size == 0u) {
        ql_error_set(error, QL_STATUS_INVALID_ARGUMENT,
                     "SMT-LIB command artifact is empty");
        return QL_STATUS_INVALID_ARGUMENT;
    }
    if (memchr(text, '\0', size) != NULL) {
        ql_error_set(error, QL_STATUS_PARSE_ERROR,
                     "SMT-LIB command artifact contains a NUL byte");
        return QL_STATUS_PARSE_ERROR;
    }

    while (cursor < size) {
        size_t token_start;
        size_t token_size;
        uint64_t depth = 1u;
        int in_string = 0;
        int in_quoted_symbol = 0;

        skip_space_and_comments(text, size, &cursor);
        if (cursor == size) {
            break;
        }
        if (text[cursor] != '(') {
            ql_error_set(error, QL_STATUS_PARSE_ERROR,
                         "SMT-LIB batch must contain top-level commands only");
            return QL_STATUS_PARSE_ERROR;
        }
        ++cursor;
        skip_space_and_comments(text, size, &cursor);
        token_start = cursor;
        while (cursor < size && !isspace((unsigned char)text[cursor]) &&
               text[cursor] != '(' && text[cursor] != ')') {
            ++cursor;
        }
        token_size = cursor - token_start;
        if (!allowed_top_level_command(text + token_start, token_size)) {
            ql_error_set(error, QL_STATUS_PARSE_ERROR,
                         "SMT-LIB top-level command is not allowed in a solver assertion batch");
            return QL_STATUS_PARSE_ERROR;
        }

        while (cursor < size && depth != 0u) {
            const char ch = text[cursor++];

            if (in_string) {
                if (ch == '"') {
                    if (cursor < size && text[cursor] == '"') {
                        ++cursor;
                    } else {
                        in_string = 0;
                    }
                }
                continue;
            }
            if (in_quoted_symbol) {
                if (ch == '|') {
                    in_quoted_symbol = 0;
                }
                continue;
            }
            if (ch == ';') {
                while (cursor < size && text[cursor] != '\n') {
                    ++cursor;
                }
            } else if (ch == '"') {
                in_string = 1;
            } else if (ch == '|') {
                in_quoted_symbol = 1;
            } else if (ch == '(') {
                if (depth == UINT64_MAX) {
                    ql_error_set(error, QL_STATUS_PARSE_ERROR,
                                 "SMT-LIB nesting depth overflow");
                    return QL_STATUS_PARSE_ERROR;
                }
                ++depth;
            } else if (ch == ')') {
                --depth;
            }
        }
        if (depth != 0u || in_string || in_quoted_symbol) {
            ql_error_set(error, QL_STATUS_PARSE_ERROR,
                         "SMT-LIB command artifact is incomplete");
            return QL_STATUS_PARSE_ERROR;
        }
    }
    ql_error_clear(error);
    return QL_STATUS_OK;
}

void QL_CALL ql_solver_capability_init(
    ql_solver_capability_v1 *capability) {
    if (capability == NULL) {
        return;
    }
    memset(capability, 0, sizeof(*capability));
    capability->struct_size = sizeof(*capability);
    capability->abi_version = QL_SOLVER_ABI_VERSION;
}

void QL_CALL ql_solver_check_request_init(
    ql_solver_check_request_v1 *request, ql_solver_logic logic) {
    if (request == NULL) {
        return;
    }
    memset(request, 0, sizeof(*request));
    request->struct_size = sizeof(*request);
    request->abi_version = QL_SOLVER_ABI_VERSION;
    request->logic = logic;
    request->maximum_bv_width = logic_uses_bv(logic) ? 64u : 0u;
    request->artifact_requests =
        QL_SOLVER_REQUEST_UNSAT_METADATA | QL_SOLVER_REQUEST_DIAGNOSTICS;
    request->stdout_limit_bytes = QL_SOLVER_MAX_STDOUT_BYTES;
    request->stderr_limit_bytes = QL_SOLVER_MAX_STDERR_BYTES;
}

void QL_CALL ql_solver_check_result_init(
    ql_solver_check_result_v1 *result) {
    if (result == NULL) {
        return;
    }
    memset(result, 0, sizeof(*result));
    result->struct_size = sizeof(*result);
    result->abi_version = QL_SOLVER_ABI_VERSION;
}

void QL_CALL ql_solver_check_result_clear(
    ql_solver_check_result_v1 *result) {
    const size_t minimum_size =
        offsetof(ql_solver_check_result_v1, reserved);

    if (result == NULL) {
        return;
    }
    if (result->abi_version == QL_SOLVER_ABI_VERSION &&
        result->struct_size >= minimum_size) {
        ql_artifact_release(result->model_artifact);
        ql_artifact_release(result->proof_artifact);
        ql_artifact_release(result->unsat_metadata_artifact);
        ql_artifact_release(result->diagnostics_artifact);
    }
    ql_solver_check_result_init(result);
}

ql_solver_unknown_reason QL_CALL ql_solver_unknown_reason_classify(
    uint32_t cancellation_observed, uint32_t watchdog_timeout_observed) {
    if (cancellation_observed != 0u) {
        return QL_SOLVER_UNKNOWN_CANCELLED;
    }
    if (watchdog_timeout_observed != 0u) {
        return QL_SOLVER_UNKNOWN_TIMEOUT;
    }
    return QL_SOLVER_UNKNOWN_BACKEND;
}

ql_status QL_CALL ql_solver_capability_validate(
    const ql_solver_capability_v1 *capability, ql_error *error) {
    const size_t minimum_size =
        offsetof(ql_solver_capability_v1, reserved);
    const uint64_t array_logics =
        QL_SOLVER_LOGIC_QF_ABV | QL_SOLVER_LOGIC_QF_AUFBV;
    const uint64_t fp_logics =
        QL_SOLVER_LOGIC_QF_FP | QL_SOLVER_LOGIC_QF_BVFP;

    if (capability == NULL) {
        ql_error_set(error, QL_STATUS_INVALID_ARGUMENT,
                     "solver capability is required");
        return QL_STATUS_INVALID_ARGUMENT;
    }
    if (capability->abi_version != QL_SOLVER_ABI_VERSION ||
        capability->struct_size < minimum_size) {
        ql_error_set(error, QL_STATUS_ABI_MISMATCH,
                     "solver capability has an incompatible ABI or structure size");
        return QL_STATUS_ABI_MISMATCH;
    }
    if (capability->availability != QL_SOLVER_AVAILABLE &&
        capability->availability != QL_SOLVER_UNAVAILABLE) {
        ql_error_set(error, QL_STATUS_INVALID_ARGUMENT,
                     "solver availability value is invalid");
        return QL_STATUS_INVALID_ARGUMENT;
    }
    if (capability->supported_logics == 0u ||
        (capability->supported_logics & ~QL_SOLVER_LOGIC_ALL) != 0u ||
        (capability->features & ~QL_SOLVER_FEATURE_ALL) != 0u) {
        ql_error_set(error, QL_STATUS_INVALID_ARGUMENT,
                     "solver capability contains invalid logic or feature bits");
        return QL_STATUS_INVALID_ARGUMENT;
    }
    if (logic_uses_bv(capability->supported_logics)) {
        if (capability->minimum_bv_width == 0u ||
            capability->maximum_bv_width < capability->minimum_bv_width) {
            ql_error_set(error, QL_STATUS_INVALID_ARGUMENT,
                         "solver bit-vector width range is invalid");
            return QL_STATUS_INVALID_ARGUMENT;
        }
    } else if (capability->minimum_bv_width != 0u ||
               capability->maximum_bv_width != 0u) {
        ql_error_set(error, QL_STATUS_INVALID_ARGUMENT,
                     "solver without bit-vector logics must advertise zero bit-vector widths");
        return QL_STATUS_INVALID_ARGUMENT;
    }
    if ((capability->supported_logics & array_logics) != 0u &&
        (capability->features & QL_SOLVER_FEATURE_ARRAYS) == 0u) {
        ql_error_set(error, QL_STATUS_INVALID_ARGUMENT,
                     "array logic support requires the arrays feature");
        return QL_STATUS_INVALID_ARGUMENT;
    }
    if ((capability->supported_logics & fp_logics) != 0u &&
        (capability->features & QL_SOLVER_FEATURE_FLOATING_POINT) == 0u) {
        ql_error_set(error, QL_STATUS_INVALID_ARGUMENT,
                     "floating-point logic support requires the floating-point feature");
        return QL_STATUS_INVALID_ARGUMENT;
    }
    ql_error_clear(error);
    return QL_STATUS_OK;
}

ql_status QL_CALL ql_solver_descriptor_validate(
    const ql_solver_descriptor_v1 *descriptor, ql_error *error) {
    const size_t minimum_size =
        offsetof(ql_solver_descriptor_v1, reserved);
    ql_status status;

    if (descriptor == NULL) {
        ql_error_set(error, QL_STATUS_INVALID_ARGUMENT,
                     "solver descriptor is required");
        return QL_STATUS_INVALID_ARGUMENT;
    }
    if (descriptor->abi_version != QL_SOLVER_ABI_VERSION ||
        descriptor->struct_size < minimum_size) {
        ql_error_set(error, QL_STATUS_ABI_MISMATCH,
                     "solver descriptor has an incompatible ABI or structure size");
        return QL_STATUS_ABI_MISMATCH;
    }
    if (descriptor->name == NULL || descriptor->name[0] == '\0' ||
        descriptor->version == NULL || descriptor->version[0] == '\0' ||
        descriptor->create == NULL || descriptor->destroy == NULL ||
        descriptor->add_smt2 == NULL || descriptor->push == NULL ||
        descriptor->pop == NULL || descriptor->check == NULL) {
        ql_error_set(error, QL_STATUS_INVALID_ARGUMENT,
                     "solver descriptor is missing identity or callbacks");
        return QL_STATUS_INVALID_ARGUMENT;
    }
    status = ql_solver_capability_validate(&descriptor->capability, error);
    if (status != QL_STATUS_OK) {
        return status;
    }
    ql_error_clear(error);
    return QL_STATUS_OK;
}

ql_status QL_CALL ql_solver_check_request_validate(
    const ql_solver_capability_v1 *capability,
    const ql_solver_check_request_v1 *request, ql_error *error) {
    const size_t minimum_size =
        offsetof(ql_solver_check_request_v1, reserved);
    ql_status status;

    status = ql_solver_capability_validate(capability, error);
    if (status != QL_STATUS_OK) {
        return status;
    }
    if (request == NULL) {
        ql_error_set(error, QL_STATUS_INVALID_ARGUMENT,
                     "solver check request is required");
        return QL_STATUS_INVALID_ARGUMENT;
    }
    if (request->abi_version != QL_SOLVER_ABI_VERSION ||
        request->struct_size < minimum_size) {
        ql_error_set(error, QL_STATUS_ABI_MISMATCH,
                     "solver check request has an incompatible ABI or structure size");
        return QL_STATUS_ABI_MISMATCH;
    }
    if (!logic_is_single(request->logic) ||
        (request->artifact_requests & ~QL_SOLVER_REQUEST_ALL) != 0u ||
        (request->required_features & ~QL_SOLVER_FEATURE_ALL) != 0u ||
        (request->cancel_state != NULL && request->is_cancelled == NULL) ||
        request->stdout_limit_bytes == 0u ||
        request->stdout_limit_bytes > QL_SOLVER_MAX_STDOUT_BYTES ||
        request->stderr_limit_bytes == 0u ||
        request->stderr_limit_bytes > QL_SOLVER_MAX_STDERR_BYTES) {
        ql_error_set(error, QL_STATUS_INVALID_ARGUMENT,
                     "solver check request contains invalid logic, artifact, feature, cancellation, or output-limit fields");
        return QL_STATUS_INVALID_ARGUMENT;
    }
    if ((capability->supported_logics & request->logic) == 0u) {
        ql_error_set(error, QL_STATUS_TYPE_MISMATCH,
                     "solver does not support the requested logic");
        return QL_STATUS_TYPE_MISMATCH;
    }
    if (logic_uses_bv(request->logic) &&
        (request->maximum_bv_width < capability->minimum_bv_width ||
         request->maximum_bv_width > capability->maximum_bv_width)) {
        ql_error_set(error, QL_STATUS_TYPE_MISMATCH,
                     "requested bit-vector width is outside the solver capability");
        return QL_STATUS_TYPE_MISMATCH;
    }
    if (!logic_uses_bv(request->logic) && request->maximum_bv_width != 0u) {
        ql_error_set(error, QL_STATUS_INVALID_ARGUMENT,
                     "a non-bit-vector request must use maximum_bv_width zero");
        return QL_STATUS_INVALID_ARGUMENT;
    }
    if ((request->required_features & ~capability->features) != 0u ||
        ((request->artifact_requests & QL_SOLVER_REQUEST_MODEL) != 0u &&
         (capability->features & QL_SOLVER_FEATURE_MODELS) == 0u) ||
        ((request->artifact_requests & QL_SOLVER_REQUEST_PROOF) != 0u &&
         (capability->features & QL_SOLVER_FEATURE_PROOFS) == 0u) ||
        (request->timeout_ms != 0u &&
         (capability->features & QL_SOLVER_FEATURE_TIMEOUT) == 0u) ||
        (request->memory_limit_mb != 0u &&
         (capability->features & QL_SOLVER_FEATURE_MEMORY_LIMIT) == 0u) ||
        (request->is_cancelled != NULL &&
         (capability->features & QL_SOLVER_FEATURE_CANCELLATION) == 0u)) {
        ql_error_set(error, QL_STATUS_TYPE_MISMATCH,
                     "solver cannot satisfy every requested feature or artifact");
        return QL_STATUS_TYPE_MISMATCH;
    }
    ql_error_clear(error);
    return QL_STATUS_OK;
}

static ql_status validate_artifact_kind(const ql_artifact *artifact,
                                        const char *expected_kind,
                                        ql_error *error) {
    ql_artifact_view view;
    ql_status status;

    if (artifact == NULL) {
        ql_error_set(error, QL_STATUS_INVALID_ARGUMENT,
                     "required solver artifact is missing");
        return QL_STATUS_INVALID_ARGUMENT;
    }
    memset(&view, 0, sizeof(view));
    view.struct_size = sizeof(view);
    status = ql_artifact_get_view(artifact, &view, error);
    if (status != QL_STATUS_OK) {
        return status;
    }
    if (strcmp(view.kind, expected_kind) != 0) {
        ql_error_set(error, QL_STATUS_TYPE_MISMATCH,
                     "solver artifact kind '%s' is not '%s'", view.kind,
                     expected_kind);
        return QL_STATUS_TYPE_MISMATCH;
    }
    ql_error_clear(error);
    return QL_STATUS_OK;
}

static int digest_is_zero(const ql_digest *digest) {
    size_t index;
    uint8_t value = 0u;

    for (index = 0u; index < QL_DIGEST_SIZE; ++index) {
        value = (uint8_t)(value | digest->bytes[index]);
    }
    return value == 0u;
}

ql_status QL_CALL ql_solver_check_result_validate(
    const ql_solver_check_request_v1 *request,
    const ql_solver_check_result_v1 *result, ql_error *error) {
    const size_t minimum_size =
        offsetof(ql_solver_check_result_v1, reserved);
    ql_status status;

    if (request == NULL || result == NULL) {
        ql_error_set(error, QL_STATUS_INVALID_ARGUMENT,
                     "solver request and result are required");
        return QL_STATUS_INVALID_ARGUMENT;
    }
    if (result->abi_version != QL_SOLVER_ABI_VERSION ||
        result->struct_size < minimum_size) {
        ql_error_set(error, QL_STATUS_ABI_MISMATCH,
                     "solver check result has an incompatible ABI or structure size");
        return QL_STATUS_ABI_MISMATCH;
    }
    if (result->backend_name == NULL || result->backend_name[0] == '\0' ||
        result->backend_version == NULL ||
        result->backend_version[0] == '\0' ||
        digest_is_zero(&result->backend_binary_digest)) {
        ql_error_set(error, QL_STATUS_INVALID_ARGUMENT,
                     "solver result is missing backend identity or binary digest");
        return QL_STATUS_INVALID_ARGUMENT;
    }
    if (result->kind != QL_SOLVER_CHECK_SAT &&
        result->kind != QL_SOLVER_CHECK_UNSAT &&
        result->kind != QL_SOLVER_CHECK_UNKNOWN) {
        ql_error_set(error, QL_STATUS_INVALID_ARGUMENT,
                     "solver result kind is invalid");
        return QL_STATUS_INVALID_ARGUMENT;
    }
    if ((result->kind == QL_SOLVER_CHECK_UNKNOWN) !=
        (result->unknown_reason != QL_SOLVER_UNKNOWN_NONE)) {
        ql_error_set(error, QL_STATUS_INVALID_ARGUMENT,
                     "solver result kind and unknown reason disagree");
        return QL_STATUS_INVALID_ARGUMENT;
    }
    if (result->unknown_reason < QL_SOLVER_UNKNOWN_NONE ||
        result->unknown_reason > QL_SOLVER_UNKNOWN_BACKEND) {
        ql_error_set(error, QL_STATUS_INVALID_ARGUMENT,
                     "solver unknown reason is invalid");
        return QL_STATUS_INVALID_ARGUMENT;
    }
    if (result->kind == QL_SOLVER_CHECK_SAT) {
        if (result->proof_artifact != NULL ||
            result->unsat_metadata_artifact != NULL) {
            ql_error_set(error, QL_STATUS_TYPE_MISMATCH,
                         "SAT result cannot carry UNSAT proof or metadata");
            return QL_STATUS_TYPE_MISMATCH;
        }
        if ((request->artifact_requests & QL_SOLVER_REQUEST_MODEL) != 0u) {
            status = validate_artifact_kind(result->model_artifact,
                                            QL_ARTIFACT_KIND_SOLVER_MODEL,
                                            error);
            if (status != QL_STATUS_OK) {
                return status;
            }
        } else if (result->model_artifact != NULL) {
            ql_error_set(error, QL_STATUS_TYPE_MISMATCH,
                         "solver returned an unrequested model artifact");
            return QL_STATUS_TYPE_MISMATCH;
        }
    } else if (result->kind == QL_SOLVER_CHECK_UNSAT) {
        if (result->model_artifact != NULL) {
            ql_error_set(error, QL_STATUS_TYPE_MISMATCH,
                         "UNSAT result cannot carry a model artifact");
            return QL_STATUS_TYPE_MISMATCH;
        }
        if ((request->artifact_requests & QL_SOLVER_REQUEST_PROOF) != 0u) {
            status = validate_artifact_kind(result->proof_artifact,
                                            QL_ARTIFACT_KIND_SOLVER_PROOF,
                                            error);
            if (status != QL_STATUS_OK) {
                return status;
            }
        } else if (result->proof_artifact != NULL) {
            ql_error_set(error, QL_STATUS_TYPE_MISMATCH,
                         "solver returned an unrequested raw proof artifact");
            return QL_STATUS_TYPE_MISMATCH;
        }
        if ((request->artifact_requests &
             QL_SOLVER_REQUEST_UNSAT_METADATA) != 0u) {
            status = validate_artifact_kind(
                result->unsat_metadata_artifact,
                QL_ARTIFACT_KIND_SOLVER_UNSAT_METADATA, error);
            if (status != QL_STATUS_OK) {
                return status;
            }
        } else if (result->unsat_metadata_artifact != NULL) {
            ql_error_set(error, QL_STATUS_TYPE_MISMATCH,
                         "solver returned unrequested UNSAT metadata");
            return QL_STATUS_TYPE_MISMATCH;
        }
    } else if (result->model_artifact != NULL ||
               result->proof_artifact != NULL ||
               result->unsat_metadata_artifact != NULL) {
        ql_error_set(error, QL_STATUS_TYPE_MISMATCH,
                     "UNKNOWN result cannot carry model or UNSAT evidence");
        return QL_STATUS_TYPE_MISMATCH;
    }
    if (result->diagnostics_artifact != NULL) {
        if ((request->artifact_requests & QL_SOLVER_REQUEST_DIAGNOSTICS) ==
            0u) {
            ql_error_set(error, QL_STATUS_TYPE_MISMATCH,
                         "solver returned unrequested diagnostics");
            return QL_STATUS_TYPE_MISMATCH;
        }
        status = validate_artifact_kind(result->diagnostics_artifact,
                                        QL_ARTIFACT_KIND_SOLVER_DIAGNOSTICS,
                                        error);
        if (status != QL_STATUS_OK) {
            return status;
        }
    }
    ql_error_clear(error);
    return QL_STATUS_OK;
}

ql_status QL_CALL ql_solver_create(
    const ql_allocator *allocator, const ql_solver_descriptor_v1 *descriptor,
    const char *options_json, ql_solver **output, ql_error *error) {
    const ql_allocator *actual_allocator = resolve_allocator(allocator);
    ql_solver *solver;
    ql_status status;

    if (output == NULL || !ql_allocator_is_valid(actual_allocator)) {
        ql_error_set(error, QL_STATUS_INVALID_ARGUMENT,
                     "valid allocator and solver output are required");
        return QL_STATUS_INVALID_ARGUMENT;
    }
    *output = NULL;
    status = ql_solver_descriptor_validate(descriptor, error);
    if (status != QL_STATUS_OK) {
        return status;
    }
    if (descriptor->capability.availability != QL_SOLVER_AVAILABLE) {
        ql_error_set(error, QL_STATUS_NOT_FOUND,
                     "solver backend '%s' is unavailable in this build",
                     descriptor->name);
        return QL_STATUS_NOT_FOUND;
    }
    solver = (ql_solver *)actual_allocator->allocate(
        actual_allocator->user_data, sizeof(*solver));
    if (solver == NULL) {
        ql_error_set(error, QL_STATUS_OUT_OF_MEMORY,
                     "could not allocate solver instance");
        return QL_STATUS_OUT_OF_MEMORY;
    }
    memset(solver, 0, sizeof(*solver));
    solver->allocator = *actual_allocator;
    solver->descriptor = descriptor;
    status = descriptor->create(actual_allocator, options_json,
                                &solver->backend_state, error);
    if (status != QL_STATUS_OK) {
        actual_allocator->deallocate(actual_allocator->user_data, solver);
        return status;
    }
    *output = solver;
    ql_error_clear(error);
    return QL_STATUS_OK;
}

void QL_CALL ql_solver_destroy(ql_solver *solver) {
    ql_allocator allocator;

    if (solver == NULL) {
        return;
    }
    allocator = solver->allocator;
    solver->descriptor->destroy(solver->backend_state);
    allocator.deallocate(allocator.user_data, solver);
}

ql_status QL_CALL ql_solver_add_smt2(
    ql_solver *solver, const ql_artifact *commands, ql_error *error) {
    ql_artifact_view view;
    ql_status status;

    if (solver == NULL || commands == NULL) {
        ql_error_set(error, QL_STATUS_INVALID_ARGUMENT,
                     "solver and SMT-LIB artifact are required");
        return QL_STATUS_INVALID_ARGUMENT;
    }
    memset(&view, 0, sizeof(view));
    view.struct_size = sizeof(view);
    status = ql_artifact_get_view(commands, &view, error);
    if (status != QL_STATUS_OK) {
        return status;
    }
    if (strcmp(view.kind, QL_ARTIFACT_KIND_SMTLIB2) != 0 ||
        view.schema_version != QL_SMTLIB2_SCHEMA_VERSION) {
        ql_error_set(error, QL_STATUS_TYPE_MISMATCH,
                     "solver input must be a version-%u %s artifact",
                     QL_SMTLIB2_SCHEMA_VERSION,
                     QL_ARTIFACT_KIND_SMTLIB2);
        return QL_STATUS_TYPE_MISMATCH;
    }
    if ((uint64_t)view.size > QL_SOLVER_MAX_SMTLIB2_BYTES) {
        ql_error_set(error, QL_STATUS_INVALID_ARGUMENT,
                     "SMT-LIB artifact exceeds the hard limit of %llu bytes",
                     (unsigned long long)QL_SOLVER_MAX_SMTLIB2_BYTES);
        return QL_STATUS_INVALID_ARGUMENT;
    }
    status = validate_smt2_batch((const char *)view.data, view.size, error);
    if (status != QL_STATUS_OK) {
        return status;
    }
    return solver->descriptor->add_smt2(
        solver->backend_state, (const char *)view.data, view.size, error);
}

ql_status QL_CALL ql_solver_push(
    ql_solver *solver, uint32_t levels, ql_error *error) {
    ql_status status;

    if (solver == NULL || levels == 0u) {
        ql_error_set(error, QL_STATUS_INVALID_ARGUMENT,
                     "solver push requires a positive level count");
        return QL_STATUS_INVALID_ARGUMENT;
    }
    if ((solver->descriptor->capability.features &
         QL_SOLVER_FEATURE_INCREMENTAL) == 0u) {
        ql_error_set(error, QL_STATUS_TYPE_MISMATCH,
                     "solver does not support incremental contexts");
        return QL_STATUS_TYPE_MISMATCH;
    }
    if ((uint64_t)levels > UINT64_MAX - solver->stack_depth) {
        ql_error_set(error, QL_STATUS_INVALID_ARGUMENT,
                     "solver context depth overflow");
        return QL_STATUS_INVALID_ARGUMENT;
    }
    status = solver->descriptor->push(solver->backend_state, levels, error);
    if (status == QL_STATUS_OK) {
        solver->stack_depth += levels;
    }
    return status;
}

ql_status QL_CALL ql_solver_pop(
    ql_solver *solver, uint32_t levels, ql_error *error) {
    ql_status status;

    if (solver == NULL || levels == 0u) {
        ql_error_set(error, QL_STATUS_INVALID_ARGUMENT,
                     "solver pop requires a positive level count");
        return QL_STATUS_INVALID_ARGUMENT;
    }
    if ((uint64_t)levels > solver->stack_depth) {
        ql_error_set(error, QL_STATUS_INVALID_ARGUMENT,
                     "solver pop exceeds the current context depth");
        return QL_STATUS_INVALID_ARGUMENT;
    }
    status = solver->descriptor->pop(solver->backend_state, levels, error);
    if (status == QL_STATUS_OK) {
        solver->stack_depth -= levels;
    }
    return status;
}

ql_status QL_CALL ql_solver_check(
    ql_solver *solver, const ql_solver_check_request_v1 *request,
    ql_solver_check_result_v1 *result, ql_error *error) {
    ql_solver_check_result_v1 local_result;
    ql_status status;

    if (solver == NULL || result == NULL) {
        ql_error_set(error, QL_STATUS_INVALID_ARGUMENT,
                     "solver and result output are required");
        return QL_STATUS_INVALID_ARGUMENT;
    }
    status = ql_solver_check_request_validate(
        &solver->descriptor->capability, request, error);
    if (status != QL_STATUS_OK) {
        return status;
    }
    ql_solver_check_result_init(&local_result);
    status = solver->descriptor->check(solver->backend_state, request,
                                       &local_result, error);
    if (status != QL_STATUS_OK) {
        ql_solver_check_result_clear(&local_result);
        return status;
    }
    local_result.backend_name = solver->descriptor->name;
    local_result.backend_version = solver->descriptor->version;
    status = ql_solver_check_result_validate(request, &local_result, error);
    if (status != QL_STATUS_OK) {
        ql_solver_check_result_clear(&local_result);
        return status;
    }
    ql_solver_check_result_clear(result);
    *result = local_result;
    ql_error_clear(error);
    return QL_STATUS_OK;
}

ql_status QL_CALL ql_smt2_builder_create(
    const ql_allocator *allocator, ql_solver_logic logic,
    ql_smt2_builder **output, ql_error *error) {
    const ql_allocator *actual_allocator = resolve_allocator(allocator);
    ql_smt2_builder *builder;
    ql_status status;

    if (output == NULL || !ql_allocator_is_valid(actual_allocator) ||
        !logic_is_single(logic)) {
        ql_error_set(error, QL_STATUS_INVALID_ARGUMENT,
                     "valid allocator, logic, and SMT-LIB builder output are required");
        return QL_STATUS_INVALID_ARGUMENT;
    }
    *output = NULL;
    builder = (ql_smt2_builder *)actual_allocator->allocate(
        actual_allocator->user_data, sizeof(*builder));
    if (builder == NULL) {
        ql_error_set(error, QL_STATUS_OUT_OF_MEMORY,
                     "could not allocate SMT-LIB builder");
        return QL_STATUS_OUT_OF_MEMORY;
    }
    memset(builder, 0, sizeof(*builder));
    buffer_init(&builder->text, actual_allocator);
    builder->logic = logic;
    status = buffer_append_cstr(&builder->text, "(set-logic ", error);
    if (status == QL_STATUS_OK) {
        status = buffer_append_cstr(&builder->text, logic_name(logic), error);
    }
    if (status == QL_STATUS_OK) {
        status = buffer_append_cstr(&builder->text, ")\n", error);
    }
    if (status != QL_STATUS_OK) {
        ql_smt2_builder_destroy(builder);
        return status;
    }
    *output = builder;
    ql_error_clear(error);
    return QL_STATUS_OK;
}

void QL_CALL ql_smt2_builder_destroy(ql_smt2_builder *builder) {
    ql_allocator allocator;

    if (builder == NULL) {
        return;
    }
    allocator = builder->text.allocator;
    buffer_dispose(&builder->text);
    allocator.deallocate(allocator.user_data, builder);
}

static ql_status builder_require_space(const ql_smt2_builder *builder,
                                       size_t additional,
                                       ql_error *error) {
    if ((uint64_t)builder->text.size > QL_SOLVER_MAX_SMTLIB2_BYTES ||
        (uint64_t)additional > QL_SOLVER_MAX_SMTLIB2_BYTES -
            (uint64_t)builder->text.size) {
        ql_error_set(error, QL_STATUS_INVALID_ARGUMENT,
                     "SMT-LIB builder output exceeds the hard limit of %llu bytes",
                     (unsigned long long)QL_SOLVER_MAX_SMTLIB2_BYTES);
        return QL_STATUS_INVALID_ARGUMENT;
    }
    return QL_STATUS_OK;
}

ql_status QL_CALL ql_smt2_builder_declare_bool(
    ql_smt2_builder *builder, const char *symbol, ql_error *error) {
    ql_status status;

    if (builder == NULL || !valid_symbol(symbol)) {
        ql_error_set(error, QL_STATUS_INVALID_ARGUMENT,
                     "Boolean declaration requires a simple SMT-LIB symbol");
        return QL_STATUS_INVALID_ARGUMENT;
    }
    status = builder_require_space(
        builder, strlen("(declare-const  Bool)\n") + strlen(symbol), error);
    if (status != QL_STATUS_OK) {
        return status;
    }
    status = buffer_append_cstr(&builder->text, "(declare-const ", error);
    if (status == QL_STATUS_OK) {
        status = buffer_append_cstr(&builder->text, symbol, error);
    }
    if (status == QL_STATUS_OK) {
        status = buffer_append_cstr(&builder->text, " Bool)\n", error);
    }
    if (status == QL_STATUS_OK) {
        ql_error_clear(error);
    }
    return status;
}

ql_status QL_CALL ql_smt2_builder_declare_bv(
    ql_smt2_builder *builder, const char *symbol, uint32_t width,
    ql_error *error) {
    char width_text[16];
    int count;
    ql_status status;

    if (builder == NULL || !valid_symbol(symbol) || width == 0u) {
        ql_error_set(error, QL_STATUS_INVALID_ARGUMENT,
                     "bit-vector declaration requires a symbol and positive width");
        return QL_STATUS_INVALID_ARGUMENT;
    }
    count = snprintf(width_text, sizeof(width_text), "%u", width);
    if (count <= 0 || (size_t)count >= sizeof(width_text)) {
        ql_error_set(error, QL_STATUS_INTERNAL_ERROR,
                     "could not format bit-vector width");
        return QL_STATUS_INTERNAL_ERROR;
    }
    status = builder_require_space(
        builder, strlen("(declare-const  (_ BitVec ))\n") + strlen(symbol) +
                     (size_t)count,
        error);
    if (status != QL_STATUS_OK) {
        return status;
    }
    status = buffer_append_cstr(&builder->text, "(declare-const ", error);
    if (status == QL_STATUS_OK) {
        status = buffer_append_cstr(&builder->text, symbol, error);
    }
    if (status == QL_STATUS_OK) {
        status = buffer_append_cstr(&builder->text, " (_ BitVec ", error);
    }
    if (status == QL_STATUS_OK) {
        status = buffer_append(&builder->text, width_text, (size_t)count,
                               error);
    }
    if (status == QL_STATUS_OK) {
        status = buffer_append_cstr(&builder->text, "))\n", error);
    }
    if (status == QL_STATUS_OK) {
        ql_error_clear(error);
    }
    return status;
}

static ql_status builder_define(ql_smt2_builder *builder, const char *symbol,
                                const char *sort_prefix, const char *width,
                                size_t width_size, const char *sort_suffix,
                                const char *term, ql_error *error) {
    ql_status status = builder_require_space(
        builder,
        strlen("(define-fun  () ") + strlen(symbol) + strlen(sort_prefix) +
            width_size + strlen(sort_suffix) + strlen(term) + strlen(")\n"),
        error);

    if (status == QL_STATUS_OK) {
        status = buffer_append_cstr(&builder->text, "(define-fun ", error);
    }
    if (status == QL_STATUS_OK) {
        status = buffer_append_cstr(&builder->text, symbol, error);
    }
    if (status == QL_STATUS_OK) {
        status = buffer_append_cstr(&builder->text, " () ", error);
    }
    if (status == QL_STATUS_OK) {
        status = buffer_append_cstr(&builder->text, sort_prefix, error);
    }
    if (status == QL_STATUS_OK && width_size != 0u) {
        status = buffer_append(&builder->text, width, width_size, error);
    }
    if (status == QL_STATUS_OK) {
        status = buffer_append_cstr(&builder->text, sort_suffix, error);
    }
    if (status == QL_STATUS_OK) {
        status = buffer_append_cstr(&builder->text, term, error);
    }
    if (status == QL_STATUS_OK) {
        status = buffer_append_cstr(&builder->text, ")\n", error);
    }
    if (status == QL_STATUS_OK) {
        ql_error_clear(error);
    }
    return status;
}

ql_status QL_CALL ql_smt2_builder_define_bool(ql_smt2_builder *builder,
                                              const char *symbol,
                                              const char *boolean_term,
                                              ql_error *error) {
    if (builder == NULL || !valid_symbol(symbol) ||
        !valid_single_line_term(boolean_term)) {
        ql_error_set(error, QL_STATUS_INVALID_ARGUMENT,
                     "Boolean definition requires a simple symbol and a non-empty single-line term");
        return QL_STATUS_INVALID_ARGUMENT;
    }
    return builder_define(builder, symbol, "Bool ", NULL, 0u, "",
                          boolean_term, error);
}

ql_status QL_CALL ql_smt2_builder_define_bv(ql_smt2_builder *builder,
                                            const char *symbol,
                                            uint32_t width, const char *term,
                                            ql_error *error) {
    char width_text[16];
    int count;

    if (builder == NULL || !valid_symbol(symbol) || width == 0u ||
        !valid_single_line_term(term)) {
        ql_error_set(error, QL_STATUS_INVALID_ARGUMENT,
                     "bit-vector definition requires a symbol, positive width, and a non-empty single-line term");
        return QL_STATUS_INVALID_ARGUMENT;
    }
    count = snprintf(width_text, sizeof(width_text), "%u", width);
    if (count <= 0 || (size_t)count >= sizeof(width_text)) {
        ql_error_set(error, QL_STATUS_INTERNAL_ERROR,
                     "could not format bit-vector width");
        return QL_STATUS_INTERNAL_ERROR;
    }
    return builder_define(builder, symbol, "(_ BitVec ", width_text,
                          (size_t)count, ") ", term, error);
}

/* The array sort is the only thing this pair adds to the transcript; select
   and store appear inside ordinary definition bodies, which stay verbatim. */
static int format_array_sort(char *text, size_t capacity, uint32_t index_width,
                             uint32_t element_width) {
    int count = snprintf(text, capacity, "(Array (_ BitVec %u) (_ BitVec %u))",
                         index_width, element_width);
    return (count <= 0 || (size_t)count >= capacity) ? -1 : count;
}

ql_status QL_CALL ql_smt2_builder_declare_array(ql_smt2_builder *builder,
                                                const char *symbol,
                                                uint32_t index_width,
                                                uint32_t element_width,
                                                ql_error *error) {
    char sort[64];
    int count;
    ql_status status;

    if (builder == NULL || !valid_symbol(symbol) || index_width == 0u ||
        element_width == 0u) {
        ql_error_set(error, QL_STATUS_INVALID_ARGUMENT,
                     "array declaration requires a symbol and positive index and element widths");
        return QL_STATUS_INVALID_ARGUMENT;
    }
    count = format_array_sort(sort, sizeof(sort), index_width, element_width);
    if (count < 0) {
        ql_error_set(error, QL_STATUS_INTERNAL_ERROR,
                     "could not format the array sort");
        return QL_STATUS_INTERNAL_ERROR;
    }
    status = builder_require_space(
        builder, strlen("(declare-const  )\n") + strlen(symbol) +
                     (size_t)count,
        error);
    if (status != QL_STATUS_OK) {
        return status;
    }
    status = buffer_append_cstr(&builder->text, "(declare-const ", error);
    if (status == QL_STATUS_OK) {
        status = buffer_append_cstr(&builder->text, symbol, error);
    }
    if (status == QL_STATUS_OK) {
        status = buffer_append_cstr(&builder->text, " ", error);
    }
    if (status == QL_STATUS_OK) {
        status = buffer_append(&builder->text, sort, (size_t)count, error);
    }
    if (status == QL_STATUS_OK) {
        status = buffer_append_cstr(&builder->text, ")\n", error);
    }
    if (status == QL_STATUS_OK) {
        ql_error_clear(error);
    }
    return status;
}

ql_status QL_CALL ql_smt2_builder_define_array(ql_smt2_builder *builder,
                                               const char *symbol,
                                               uint32_t index_width,
                                               uint32_t element_width,
                                               const char *term,
                                               ql_error *error) {
    char sort[64];
    int count;

    if (builder == NULL || !valid_symbol(symbol) || index_width == 0u ||
        element_width == 0u || !valid_single_line_term(term)) {
        ql_error_set(error, QL_STATUS_INVALID_ARGUMENT,
                     "array definition requires a symbol, positive index and element widths, and a non-empty single-line term");
        return QL_STATUS_INVALID_ARGUMENT;
    }
    count = format_array_sort(sort, sizeof(sort), index_width, element_width);
    if (count < 0) {
        ql_error_set(error, QL_STATUS_INTERNAL_ERROR,
                     "could not format the array sort");
        return QL_STATUS_INTERNAL_ERROR;
    }
    return builder_define(builder, symbol, "", sort, (size_t)count, " ", term,
                          error);
}

ql_status QL_CALL ql_smt2_builder_assert(
    ql_smt2_builder *builder, const char *boolean_term, ql_error *error) {
    ql_status status;

    if (builder == NULL || !valid_single_line_term(boolean_term)) {
        ql_error_set(error, QL_STATUS_INVALID_ARGUMENT,
                     "assertion requires a non-empty single-line SMT-LIB term");
        return QL_STATUS_INVALID_ARGUMENT;
    }
    status = builder_require_space(
        builder, strlen("(assert )\n") + strlen(boolean_term), error);
    if (status != QL_STATUS_OK) {
        return status;
    }
    status = buffer_append_cstr(&builder->text, "(assert ", error);
    if (status == QL_STATUS_OK) {
        status = buffer_append_cstr(&builder->text, boolean_term, error);
    }
    if (status == QL_STATUS_OK) {
        status = buffer_append_cstr(&builder->text, ")\n", error);
    }
    if (status == QL_STATUS_OK) {
        ql_error_clear(error);
    }
    return status;
}

ql_status QL_CALL ql_smt2_builder_build(
    const ql_smt2_builder *builder, ql_artifact **output, ql_error *error) {
    if (builder == NULL || output == NULL) {
        ql_error_set(error, QL_STATUS_INVALID_ARGUMENT,
                     "SMT-LIB builder and artifact output are required");
        return QL_STATUS_INVALID_ARGUMENT;
    }
    return ql_artifact_create(
        &builder->text.allocator, QL_ARTIFACT_KIND_SMTLIB2,
        QL_SMTLIB2_SCHEMA_VERSION, builder->text.data, builder->text.size,
        output, error);
}

ql_status QL_CALL ql_solver_checked_proof_binding_validate(
    const ql_solver_check_result_v1 *result,
    const ql_solver_checked_proof_binding_v1 *binding, ql_error *error) {
    const size_t minimum_size =
        offsetof(ql_solver_checked_proof_binding_v1, reserved);
    ql_artifact_view raw_view;
    ql_status status;

    if (result == NULL || binding == NULL) {
        ql_error_set(error, QL_STATUS_INVALID_ARGUMENT,
                     "UNSAT result and checked-proof binding are required");
        return QL_STATUS_INVALID_ARGUMENT;
    }
    if (result->abi_version != QL_SOLVER_ABI_VERSION ||
        result->struct_size <
            offsetof(ql_solver_check_result_v1, reserved)) {
        ql_error_set(error, QL_STATUS_ABI_MISMATCH,
                     "solver result has an incompatible ABI or structure size");
        return QL_STATUS_ABI_MISMATCH;
    }
    if (result->kind != QL_SOLVER_CHECK_UNSAT) {
        ql_error_set(error, QL_STATUS_TYPE_MISMATCH,
                     "checked-proof binding requires an UNSAT result");
        return QL_STATUS_TYPE_MISMATCH;
    }
    if (result->proof_artifact == NULL) {
        ql_error_set(error, QL_STATUS_TYPE_MISMATCH,
                     "checked-proof binding requires a raw solver proof artifact");
        return QL_STATUS_TYPE_MISMATCH;
    }
    if (binding->abi_version != QL_SOLVER_ABI_VERSION ||
        binding->struct_size < minimum_size) {
        ql_error_set(error, QL_STATUS_ABI_MISMATCH,
                     "checked-proof binding has an incompatible ABI or structure size");
        return QL_STATUS_ABI_MISMATCH;
    }
    if (binding->checker_name == NULL ||
        binding->checker_name[0] == '\0' ||
        binding->checker_version == NULL ||
        binding->checker_version[0] == '\0' ||
        binding->checked_proof_artifact == NULL) {
        ql_error_set(error, QL_STATUS_TYPE_MISMATCH,
                     "checked-proof binding requires checker metadata and an artifact");
        return QL_STATUS_TYPE_MISMATCH;
    }
    if (!ql_digest_equal(&result->query_digest,
                         &binding->query_digest)) {
        ql_error_set(error, QL_STATUS_TYPE_MISMATCH,
                     "checked proof is bound to a different solver query");
        return QL_STATUS_TYPE_MISMATCH;
    }
    if (!ql_digest_equal(&result->backend_binary_digest,
                         &binding->backend_binary_digest)) {
        ql_error_set(error, QL_STATUS_TYPE_MISMATCH,
                     "checked proof is bound to a different solver binary");
        return QL_STATUS_TYPE_MISMATCH;
    }
    memset(&raw_view, 0, sizeof(raw_view));
    raw_view.struct_size = sizeof(raw_view);
    status = ql_artifact_get_view(result->proof_artifact, &raw_view, error);
    if (status != QL_STATUS_OK) {
        return status;
    }
    if (strcmp(raw_view.kind, QL_ARTIFACT_KIND_SOLVER_PROOF) != 0 ||
        !ql_digest_equal(&raw_view.digest,
                         &binding->raw_proof_digest)) {
        ql_error_set(error, QL_STATUS_TYPE_MISMATCH,
                     "checked proof is not bound to the returned raw solver proof");
        return QL_STATUS_TYPE_MISMATCH;
    }
    status = validate_artifact_kind(
        binding->checked_proof_artifact, QL_ARTIFACT_KIND_PROOF,
        error);
    if (status != QL_STATUS_OK) {
        return status;
    }
    ql_error_clear(error);
    return QL_STATUS_OK;
}

typedef struct ql_process_capture ql_process_capture;

typedef struct ql_process_read_stream {
    uv_pipe_t pipe;
    ql_process_capture *capture;
    ql_buffer *output;
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
    ql_buffer stdout_text;
    ql_buffer stderr_text;
    const ql_solver_check_request_v1 *request;
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

typedef struct ql_process_output {
    ql_buffer stdout_text;
    ql_buffer stderr_text;
    int64_t exit_status;
    int term_signal;
    int cancelled;
    int timed_out;
    int killed;
} ql_process_output;

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
   ql_process_capture on run_process's stack, so a refused close leaves that
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
        if (buffer_append_limited(
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
                                   "could not write SMT-LIB to solver process");
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
    if (capture->request != NULL &&
        capture->request->is_cancelled != NULL &&
        capture->request->is_cancelled(capture->request->cancel_state) != 0u) {
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

static void process_output_dispose(ql_process_output *output) {
    buffer_dispose(&output->stdout_text);
    buffer_dispose(&output->stderr_text);
    memset(output, 0, sizeof(*output));
}

static ql_status run_process(
    const ql_allocator *allocator, const char *path, char **arguments,
    const char *input, size_t input_size,
    const ql_solver_check_request_v1 *request, ql_process_output *output,
    ql_error *error) {
    ql_process_capture capture;
    uv_process_options_t options;
    uv_stdio_container_t stdio[3];
    int uv_status;
    ql_status status = QL_STATUS_OK;

    memset(output, 0, sizeof(*output));
    buffer_init(&output->stdout_text, allocator);
    buffer_init(&output->stderr_text, allocator);
    memset(&capture, 0, sizeof(capture));
    capture.allocator = *allocator;
    capture.request = request;
    capture.callback_status = QL_STATUS_OK;
    buffer_init(&capture.stdout_text, allocator);
    buffer_init(&capture.stderr_text, allocator);

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
    if (request != NULL && request->timeout_ms != 0u) {
        uint64_t deadline = capture.started_ms;
        if (request->timeout_ms >
            UINT64_MAX - deadline - QL_PROCESS_WATCHDOG_GRACE_MS) {
            deadline = UINT64_MAX;
        } else {
            deadline += request->timeout_ms +
                        QL_PROCESS_WATCHDOG_GRACE_MS;
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
    capture.child_stdout.hard_limit = request == NULL ?
        QL_SOLVER_MAX_STDOUT_BYTES : request->stdout_limit_bytes;
    capture.child_stdout.description = "solver stdout";
    capture.child_stdout.pipe.data = &capture.child_stdout;
    capture.child_stderr.capture = &capture;
    capture.child_stderr.output = &capture.stderr_text;
    capture.child_stderr.hard_limit = request == NULL ?
        QL_SOLVER_MAX_STDERR_BYTES : request->stderr_limit_bytes;
    capture.child_stderr.description = "solver stderr";
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
    options.file = path;
    options.args = arguments;
    options.stdio_count = 3;
    options.stdio = stdio;
#if defined(_WIN32)
    options.flags = UV_PROCESS_WINDOWS_HIDE;
#endif
    capture.process.data = &capture;
    uv_status = uv_spawn(capture.loop, &capture.process, &options);
    if (uv_status != 0) {
        ql_error_set(error, QL_STATUS_NOT_FOUND,
                     "could not start solver executable '%s': %s", path,
                     uv_strerror(uv_status));
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
                                       "could not submit SMT-LIB to solver process");
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

    output->stdout_text = capture.stdout_text;
    output->stderr_text = capture.stderr_text;
    memset(&capture.stdout_text, 0, sizeof(capture.stdout_text));
    memset(&capture.stderr_text, 0, sizeof(capture.stderr_text));
    output->exit_status = capture.exit_status;
    output->term_signal = capture.term_signal;
    output->cancelled = capture.cancelled;
    output->timed_out = capture.timed_out;
    output->killed = capture.killed;
    ql_error_clear(error);

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
    buffer_dispose(&capture.stdout_text);
    buffer_dispose(&capture.stderr_text);
    if (status != QL_STATUS_OK) {
        process_output_dispose(output);
    }
    return status;

drain_loop:
    (void)uv_run(capture.loop, UV_RUN_DEFAULT);
    goto close_loop;

fail_without_loop:
    buffer_dispose(&capture.stdout_text);
    buffer_dispose(&capture.stderr_text);
    process_output_dispose(output);
    return status;
}

typedef struct ql_bitwuzla_state {
    ql_allocator allocator;
    char *snapshot_directory;
    char *executable;
    ql_digest executable_digest;
    ql_buffer transcript;
} ql_bitwuzla_state;

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

static int absolute_executable_path(const char *path) {
    if (path == NULL || path[0] == '\0') {
        return 0;
    }
    if (path[0] == '/' ||
        (path[0] == '\\' && path[1] == '\\')) {
        return 1;
    }
    return isalpha((unsigned char)path[0]) && path[1] == ':' &&
           (path[2] == '/' || path[2] == '\\');
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
                     "could not open selected Bitwuzla executable for snapshotting");
        return QL_STATUS_IO_ERROR;
    }
    snapshot = open_binary_write(snapshot_path);
    if (snapshot == NULL) {
        fclose(source);
        ql_error_set(error, QL_STATUS_IO_ERROR,
                     "could not create private Bitwuzla snapshot");
        return QL_STATUS_IO_ERROR;
    }
    while ((count = fread(bytes, 1u, sizeof(bytes), source)) != 0u) {
        if (fwrite(bytes, 1u, count, snapshot) != count) {
            ql_error_set(error, QL_STATUS_IO_ERROR,
                         "could not write private Bitwuzla snapshot");
            status = QL_STATUS_IO_ERROR;
            break;
        }
    }
    if (status == QL_STATUS_OK && ferror(source)) {
        ql_error_set(error, QL_STATUS_IO_ERROR,
                     "could not read selected Bitwuzla executable while snapshotting");
        status = QL_STATUS_IO_ERROR;
    }
    if (fclose(source) != 0 && status == QL_STATUS_OK) {
        ql_error_set(error, QL_STATUS_IO_ERROR,
                     "could not close selected Bitwuzla executable after snapshotting");
        status = QL_STATUS_IO_ERROR;
    }
    if (fclose(snapshot) != 0 && status == QL_STATUS_OK) {
        ql_error_set(error, QL_STATUS_IO_ERROR,
                     "could not finalize private Bitwuzla snapshot");
        status = QL_STATUS_IO_ERROR;
    }
    return status;
}

static ql_status create_executable_snapshot(
    const ql_allocator *allocator, const char *source_path,
    char **directory_output, char **executable_output, ql_error *error) {
#if defined(_WIN32)
    static const char executable_name[] = "bitwuzla.exe";
#else
    static const char executable_name[] = "bitwuzla";
#endif
    char directory_stem[64];
    char temporary_directory[QL_SOLVER_PATH_CAPACITY];
    char directory_template[QL_SOLVER_PATH_CAPACITY];
    char snapshot_path[QL_SOLVER_PATH_CAPACITY];
    size_t temporary_size = sizeof(temporary_directory);
    char *directory = NULL;
    char *executable = NULL;
    uv_fs_t request;
    int uv_status;
    int count;
    int stem_count;
    ql_status status;

    *directory_output = NULL;
    *executable_output = NULL;
    stem_count = snprintf(directory_stem, sizeof(directory_stem),
                          "quodlibet-bitwuzla-%lld-XXXXXX",
                          (long long)uv_os_getpid());
    if (stem_count <= 0 || (size_t)stem_count >= sizeof(directory_stem)) {
        ql_error_set(error, QL_STATUS_INTERNAL_ERROR,
                     "could not format private Bitwuzla snapshot identity");
        return QL_STATUS_INTERNAL_ERROR;
    }
    uv_status = uv_os_tmpdir(temporary_directory, &temporary_size);
    if (uv_status != 0 || temporary_size == 0u ||
        temporary_size >= sizeof(temporary_directory)) {
        ql_error_set(error, QL_STATUS_IO_ERROR,
                     "could not locate a directory for the private Bitwuzla snapshot");
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
                     "private Bitwuzla snapshot path is too long");
        return QL_STATUS_INVALID_ARGUMENT;
    }
    uv_status = uv_fs_mkdtemp(NULL, &request, directory_template, NULL);
    if (uv_status < 0) {
        ql_error_set(error, QL_STATUS_IO_ERROR,
                     "could not create private Bitwuzla snapshot directory: %s",
                     uv_strerror(uv_status));
        uv_fs_req_cleanup(&request);
        return QL_STATUS_IO_ERROR;
    }
    if (strlen(request.path) >= sizeof(directory_template)) {
        (void)remove(request.path);
        uv_fs_req_cleanup(&request);
        ql_error_set(error, QL_STATUS_INVALID_ARGUMENT,
                     "private Bitwuzla snapshot directory path is too long");
        return QL_STATUS_INVALID_ARGUMENT;
    }
    memcpy(directory_template, request.path, strlen(request.path) + 1u);
    directory = copy_bytes_as_cstr(allocator, directory_template,
                                   strlen(directory_template));
    uv_fs_req_cleanup(&request);
    if (directory == NULL) {
        cleanup_snapshot_paths(NULL, directory_template);
        ql_error_set(error, QL_STATUS_OUT_OF_MEMORY,
                     "could not retain private Bitwuzla snapshot directory");
        return QL_STATUS_OUT_OF_MEMORY;
    }
#if !defined(_WIN32)
    if (chmod(directory, S_IRWXU) != 0) {
        cleanup_snapshot_paths(NULL, directory);
        allocator->deallocate(allocator->user_data, directory);
        ql_error_set(error, QL_STATUS_IO_ERROR,
                     "could not make Bitwuzla snapshot directory private");
        return QL_STATUS_IO_ERROR;
    }
#endif
    count = snprintf(snapshot_path, sizeof(snapshot_path), "%s/%s",
                     directory, executable_name);
    if (count <= 0 || (size_t)count >= sizeof(snapshot_path)) {
        cleanup_snapshot_paths(NULL, directory);
        allocator->deallocate(allocator->user_data, directory);
        ql_error_set(error, QL_STATUS_INVALID_ARGUMENT,
                     "private Bitwuzla executable path is too long");
        return QL_STATUS_INVALID_ARGUMENT;
    }
    executable = copy_bytes_as_cstr(allocator, snapshot_path,
                                    (size_t)count);
    if (executable == NULL) {
        cleanup_snapshot_paths(NULL, directory);
        allocator->deallocate(allocator->user_data, directory);
        ql_error_set(error, QL_STATUS_OUT_OF_MEMORY,
                     "could not retain private Bitwuzla executable path");
        return QL_STATUS_OUT_OF_MEMORY;
    }
    status = copy_snapshot_bytes(source_path, executable, error);
#if defined(_WIN32)
    if (status == QL_STATUS_OK) {
        const int chmod_status =
            uv_fs_chmod(NULL, &request, executable, 0444, NULL);
        uv_fs_req_cleanup(&request);
        if (chmod_status < 0) {
            ql_error_set(error, QL_STATUS_IO_ERROR,
                         "could not mark private Bitwuzla snapshot read-only: %s",
                         uv_strerror(chmod_status));
            status = QL_STATUS_IO_ERROR;
        }
    }
#else
    if (status == QL_STATUS_OK && chmod(executable, S_IRUSR | S_IXUSR) != 0) {
        ql_error_set(error, QL_STATUS_IO_ERROR,
                     "could not make private Bitwuzla snapshot executable");
        status = QL_STATUS_IO_ERROR;
    }
    if (status == QL_STATUS_OK &&
        chmod(directory, S_IRUSR | S_IXUSR) != 0) {
        ql_error_set(error, QL_STATUS_IO_ERROR,
                     "could not make Bitwuzla snapshot directory read-only");
        status = QL_STATUS_IO_ERROR;
    }
#endif
    if (status != QL_STATUS_OK) {
        cleanup_snapshot_paths(executable, directory);
        allocator->deallocate(allocator->user_data, executable);
        allocator->deallocate(allocator->user_data, directory);
        return status;
    }
    *directory_output = directory;
    *executable_output = executable;
    ql_error_clear(error);
    return QL_STATUS_OK;
}

static int readable_file(const char *path) {
    FILE *file = open_binary_read(path);
    if (file == NULL) {
        return 0;
    }
    fclose(file);
    return 1;
}

static ql_status adjacent_bitwuzla_path(
    const ql_allocator *allocator, char **output, ql_error *error) {
    char host_path[QL_SOLVER_PATH_CAPACITY];
    size_t host_size = sizeof(host_path);
    size_t separator;
#if defined(_WIN32)
    static const char executable_name[] = "bitwuzla.exe";
#else
    static const char executable_name[] = "bitwuzla";
#endif
    const size_t name_size = sizeof(executable_name) - 1u;
    int uv_status;

    *output = NULL;
    uv_status = uv_exepath(host_path, &host_size);
    if (uv_status != 0 || host_size == 0u || host_size >= sizeof(host_path)) {
        return QL_STATUS_OK;
    }
    separator = host_size;
    while (separator != 0u && host_path[separator - 1u] != '/' &&
           host_path[separator - 1u] != '\\') {
        --separator;
    }
    if (separator == 0u ||
        separator > sizeof(host_path) - name_size - 1u) {
        return QL_STATUS_OK;
    }
    memcpy(host_path + separator, executable_name, name_size);
    host_path[separator + name_size] = '\0';
    if (!readable_file(host_path)) {
        return QL_STATUS_OK;
    }
    *output = copy_bytes_as_cstr(
        allocator, host_path, separator + name_size);
    if (*output == NULL) {
        ql_error_set(error, QL_STATUS_OUT_OF_MEMORY,
                     "could not copy adjacent Bitwuzla executable path");
        return QL_STATUS_OUT_OF_MEMORY;
    }
    return QL_STATUS_OK;
}

static ql_status bitwuzla_select_executable(
    const ql_allocator *allocator, const char *options_json, char **output,
    ql_error *error) {
    const char *selected = NULL;
    size_t selected_size = 0u;
    yyjson_doc *document = NULL;
    yyjson_val *root;
    yyjson_val *value;
    yyjson_read_err read_error;

    *output = NULL;
    if (options_json != NULL && options_json[0] != '\0') {
        memset(&read_error, 0, sizeof(read_error));
        document = yyjson_read_opts((char *)(uintptr_t)options_json,
                                    strlen(options_json), 0u, NULL,
                                    &read_error);
        if (document == NULL) {
            ql_error_set(error, QL_STATUS_PARSE_ERROR,
                         "Bitwuzla options JSON is invalid at byte %zu: %s",
                         read_error.pos,
                         read_error.msg == NULL ? "parse error" :
                                                  read_error.msg);
            return QL_STATUS_PARSE_ERROR;
        }
        root = yyjson_doc_get_root(document);
        value = yyjson_is_obj(root) ? yyjson_obj_get(root, "executable") :
                                      NULL;
        if (!yyjson_is_obj(root) || yyjson_obj_size(root) > 1u ||
            (yyjson_obj_size(root) == 1u && !yyjson_is_str(value)) ||
            (yyjson_is_str(value) && yyjson_get_len(value) == 0u)) {
            yyjson_doc_free(document);
            ql_error_set(error, QL_STATUS_INVALID_ARGUMENT,
                         "Bitwuzla options must be {} or contain only a non-empty string field 'executable'");
            return QL_STATUS_INVALID_ARGUMENT;
        }
        if (yyjson_is_str(value)) {
            selected = yyjson_get_str(value);
            selected_size = yyjson_get_len(value);
        }
    }
    if (selected == NULL) {
        ql_status adjacent_status = adjacent_bitwuzla_path(
            allocator, output, error);
        if (adjacent_status != QL_STATUS_OK || *output != NULL) {
            yyjson_doc_free(document);
            return adjacent_status;
        }
        selected = QL_BITWUZLA_EXECUTABLE;
        selected_size = strlen(selected);
    }
    if (selected_size == 0u) {
        yyjson_doc_free(document);
        ql_error_set(error, QL_STATUS_NOT_FOUND,
                     "Bitwuzla executable is not configured; set the build default or options_json executable");
        return QL_STATUS_NOT_FOUND;
    }
    if (!absolute_executable_path(selected)) {
        yyjson_doc_free(document);
        ql_error_set(error, QL_STATUS_INVALID_ARGUMENT,
                     "Bitwuzla executable path must be absolute");
        return QL_STATUS_INVALID_ARGUMENT;
    }
    *output = copy_bytes_as_cstr(allocator, selected, selected_size);
    yyjson_doc_free(document);
    if (*output == NULL) {
        ql_error_set(error, QL_STATUS_OUT_OF_MEMORY,
                     "could not copy Bitwuzla executable path");
        return QL_STATUS_OUT_OF_MEMORY;
    }
    ql_error_clear(error);
    return QL_STATUS_OK;
}

/* A snapshot is hashed immediately after it is written, and on Windows a
   freshly written executable is briefly held by on-access antivirus scans, so
   the first open can fail with a sharing violation even when we deny no
   sharing ourselves: the scanner's handle can be the exclusive one. Measured
   on an 8-way concurrent batch, 11 of 320 checks failed at 200ms of retry and
   5 of 320 still failed with shared-read opens, so the window under load is
   seconds, not milliseconds. Three seconds of bounded retry costs nothing on
   the uncontended path; a path that stays unopenable still fails. */
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

static ql_status digest_executable(const char *path, ql_digest *digest,
                                   ql_error *error) {
    unsigned char bytes[65536];
    blake3_hasher hasher;
    FILE *file = open_binary_read_retry(path);
    size_t count;

    if (file == NULL) {
        ql_error_set(error, QL_STATUS_IO_ERROR,
                     "could not open Bitwuzla executable for identity hashing");
        return QL_STATUS_IO_ERROR;
    }
    blake3_hasher_init(&hasher);
    while ((count = fread(bytes, 1u, sizeof(bytes), file)) != 0u) {
        blake3_hasher_update(&hasher, bytes, count);
    }
    if (ferror(file)) {
        fclose(file);
        ql_error_set(error, QL_STATUS_IO_ERROR,
                     "could not read Bitwuzla executable for identity hashing");
        return QL_STATUS_IO_ERROR;
    }
    if (fclose(file) != 0) {
        ql_error_set(error, QL_STATUS_IO_ERROR,
                     "could not close Bitwuzla executable after identity hashing");
        return QL_STATUS_IO_ERROR;
    }
    blake3_hasher_finalize(&hasher, digest->bytes, QL_DIGEST_SIZE);
    ql_error_clear(error);
    return QL_STATUS_OK;
}

static ql_status verify_snapshot_digest(
    const ql_bitwuzla_state *state, const char *phase, ql_error *error) {
    ql_digest observed;
    ql_status status = digest_executable(state->executable, &observed, error);

    if (status != QL_STATUS_OK) {
        return status;
    }
    if (!ql_digest_equal(&state->executable_digest, &observed)) {
        ql_error_set(error, QL_STATUS_ABI_MISMATCH,
                     "Bitwuzla snapshot content changed %s", phase);
        return QL_STATUS_ABI_MISMATCH;
    }
    ql_error_clear(error);
    return QL_STATUS_OK;
}

static int exact_bitwuzla_version(const ql_buffer *stdout_text) {
    static const char lf[] = QL_BITWUZLA_VERSION "\n";
    static const char crlf[] = QL_BITWUZLA_VERSION "\r\n";

    return (stdout_text->size == sizeof(lf) - 1u &&
            memcmp(stdout_text->data, lf, sizeof(lf) - 1u) == 0) ||
           (stdout_text->size == sizeof(crlf) - 1u &&
            memcmp(stdout_text->data, crlf, sizeof(crlf) - 1u) == 0);
}

static ql_status bitwuzla_verify_version(
    const ql_allocator *allocator, const char *executable, ql_error *error) {
    char *arguments[3];
    ql_process_output output;
    ql_solver_check_request_v1 probe_request;
    ql_status status;

    arguments[0] = (char *)(uintptr_t)executable;
    arguments[1] = (char *)"--version";
    arguments[2] = NULL;
    ql_solver_check_request_init(&probe_request, QL_SOLVER_LOGIC_QF_BV);
    probe_request.artifact_requests = 0u;
    probe_request.timeout_ms = QL_BITWUZLA_VERSION_TIMEOUT_MS -
                               QL_PROCESS_WATCHDOG_GRACE_MS;
    status = run_process(allocator, executable, arguments, NULL, 0u,
                         &probe_request, &output, error);
    if (status != QL_STATUS_OK) {
        return status;
    }
    if (output.timed_out) {
        process_output_dispose(&output);
        ql_error_set(error, QL_STATUS_METHOD_ERROR,
                     "Bitwuzla version probe exceeded its 5 second deadline");
        return QL_STATUS_METHOD_ERROR;
    }
    if (output.exit_status != 0 || output.term_signal != 0 ||
        output.stderr_text.size != 0u ||
        !exact_bitwuzla_version(&output.stdout_text)) {
        process_output_dispose(&output);
        ql_error_set(error, QL_STATUS_ABI_MISMATCH,
                     "configured solver is not the pinned Bitwuzla %s executable",
                     QL_BITWUZLA_VERSION);
        return QL_STATUS_ABI_MISMATCH;
    }
    process_output_dispose(&output);
    ql_error_clear(error);
    return QL_STATUS_OK;
}

static ql_status QL_CALL bitwuzla_create(
    const ql_allocator *allocator, const char *options_json,
    void **backend_state, ql_error *error) {
    ql_bitwuzla_state *state;
    char *selected_executable = NULL;
    ql_status status;

    if (backend_state == NULL) {
        ql_error_set(error, QL_STATUS_INVALID_ARGUMENT,
                     "Bitwuzla backend state output is required");
        return QL_STATUS_INVALID_ARGUMENT;
    }
    *backend_state = NULL;
    state = (ql_bitwuzla_state *)allocator->allocate(
        allocator->user_data, sizeof(*state));
    if (state == NULL) {
        ql_error_set(error, QL_STATUS_OUT_OF_MEMORY,
                     "could not allocate Bitwuzla backend state");
        return QL_STATUS_OUT_OF_MEMORY;
    }
    memset(state, 0, sizeof(*state));
    state->allocator = *allocator;
    buffer_init(&state->transcript, allocator);
    status = bitwuzla_select_executable(
        allocator, options_json, &selected_executable, error);
    if (status == QL_STATUS_OK) {
        status = create_executable_snapshot(
            allocator, selected_executable, &state->snapshot_directory,
            &state->executable, error);
    }
    allocator->deallocate(allocator->user_data, selected_executable);
    if (status == QL_STATUS_OK) {
        status = digest_executable(state->executable,
                                   &state->executable_digest, error);
    }
    if (status == QL_STATUS_OK) {
        const ql_status probe_status = bitwuzla_verify_version(
            allocator, state->executable, error);
        ql_error probe_error;
        ql_status integrity_status;

        if (error == NULL) {
            ql_error_clear(&probe_error);
        } else {
            probe_error = *error;
        }
        integrity_status = verify_snapshot_digest(
            state, "during the version probe", error);
        if (integrity_status != QL_STATUS_OK) {
            status = integrity_status;
        } else {
            status = probe_status;
            if (status != QL_STATUS_OK && error != NULL) {
                *error = probe_error;
            }
        }
    }
    if (status != QL_STATUS_OK) {
        cleanup_snapshot_paths(state->executable,
                               state->snapshot_directory);
        buffer_dispose(&state->transcript);
        allocator->deallocate(allocator->user_data, state->executable);
        allocator->deallocate(allocator->user_data,
                              state->snapshot_directory);
        allocator->deallocate(allocator->user_data, state);
        return status;
    }
    *backend_state = state;
    ql_error_clear(error);
    return QL_STATUS_OK;
}

static void QL_CALL bitwuzla_destroy(void *backend_state) {
    ql_bitwuzla_state *state = (ql_bitwuzla_state *)backend_state;
    ql_allocator allocator;

    if (state == NULL) {
        return;
    }
    allocator = state->allocator;
    buffer_dispose(&state->transcript);
    cleanup_snapshot_paths(state->executable, state->snapshot_directory);
    allocator.deallocate(allocator.user_data, state->executable);
    allocator.deallocate(allocator.user_data, state->snapshot_directory);
    allocator.deallocate(allocator.user_data, state);
}

static ql_status QL_CALL bitwuzla_add_smt2(
    void *backend_state, const char *commands, size_t size,
    ql_error *error) {
    ql_bitwuzla_state *state = (ql_bitwuzla_state *)backend_state;
    ql_status status;

    status = buffer_append_limited(
        &state->transcript, commands, size, QL_SOLVER_MAX_SMTLIB2_BYTES,
        "Bitwuzla SMT-LIB transcript", error);
    if (status == QL_STATUS_OK && commands[size - 1u] != '\n') {
        status = buffer_append_limited(
            &state->transcript, "\n", 1u, QL_SOLVER_MAX_SMTLIB2_BYTES,
            "Bitwuzla SMT-LIB transcript", error);
    }
    if (status == QL_STATUS_OK) {
        ql_error_clear(error);
    }
    return status;
}

static ql_status bitwuzla_append_stack_command(
    ql_bitwuzla_state *state, const char *command, uint32_t levels,
    ql_error *error) {
    char levels_text[16];
    int count;
    ql_status status;

    count = snprintf(levels_text, sizeof(levels_text), "%u", levels);
    if (count <= 0 || (size_t)count >= sizeof(levels_text)) {
        ql_error_set(error, QL_STATUS_INTERNAL_ERROR,
                     "could not format solver context level count");
        return QL_STATUS_INTERNAL_ERROR;
    }
    status = buffer_append_limited(
        &state->transcript, "(", 1u, QL_SOLVER_MAX_SMTLIB2_BYTES,
        "Bitwuzla SMT-LIB transcript", error);
    if (status == QL_STATUS_OK) {
        status = buffer_append_limited(
            &state->transcript, command, strlen(command),
            QL_SOLVER_MAX_SMTLIB2_BYTES,
            "Bitwuzla SMT-LIB transcript", error);
    }
    if (status == QL_STATUS_OK) {
        status = buffer_append_limited(
            &state->transcript, " ", 1u, QL_SOLVER_MAX_SMTLIB2_BYTES,
            "Bitwuzla SMT-LIB transcript", error);
    }
    if (status == QL_STATUS_OK) {
        status = buffer_append_limited(
            &state->transcript, levels_text, (size_t)count,
            QL_SOLVER_MAX_SMTLIB2_BYTES,
            "Bitwuzla SMT-LIB transcript", error);
    }
    if (status == QL_STATUS_OK) {
        status = buffer_append_limited(
            &state->transcript, ")\n", 2u, QL_SOLVER_MAX_SMTLIB2_BYTES,
            "Bitwuzla SMT-LIB transcript", error);
    }
    if (status == QL_STATUS_OK) {
        ql_error_clear(error);
    }
    return status;
}

static ql_status QL_CALL bitwuzla_push(
    void *backend_state, uint32_t levels, ql_error *error) {
    return bitwuzla_append_stack_command(
        (ql_bitwuzla_state *)backend_state, "push", levels, error);
}

static ql_status QL_CALL bitwuzla_pop(
    void *backend_state, uint32_t levels, ql_error *error) {
    return bitwuzla_append_stack_command(
        (ql_bitwuzla_state *)backend_state, "pop", levels, error);
}

static ql_status create_text_artifact(
    const ql_allocator *allocator, const char *kind, const char *data,
    size_t size, ql_artifact **output, ql_error *error) {
    return ql_artifact_create(allocator, kind,
                              QL_SOLVER_ARTIFACT_SCHEMA_VERSION, data, size,
                              output, error);
}

static ql_status create_unsat_metadata(
    const ql_bitwuzla_state *state,
    const ql_solver_check_result_v1 *result, int64_t exit_status,
    ql_artifact **output, ql_error *error) {
    ql_buffer metadata;
    char digest[QL_DIGEST_HEX_SIZE];
    char binary_digest[QL_DIGEST_HEX_SIZE];
    char status_text[32];
    int count;
    ql_status status;

    buffer_init(&metadata, &state->allocator);
    ql_digest_hex(&result->query_digest, digest);
    ql_digest_hex(&result->backend_binary_digest, binary_digest);
    count = snprintf(status_text, sizeof(status_text), "%lld",
                     (long long)exit_status);
    if (count <= 0 || (size_t)count >= sizeof(status_text)) {
        ql_error_set(error, QL_STATUS_INTERNAL_ERROR,
                     "could not format Bitwuzla exit status");
        buffer_dispose(&metadata);
        return QL_STATUS_INTERNAL_ERROR;
    }
    status = buffer_append_cstr(
        &metadata,
        "schema=quodlibet.solver-unsat-metadata.v1\n"
        "backend=bitwuzla\nversion=" QL_BITWUZLA_VERSION "\n"
        "transport=process\nbinary_digest=",
        error);
    if (status == QL_STATUS_OK) {
        status = buffer_append_cstr(&metadata, binary_digest, error);
    }
    if (status == QL_STATUS_OK) {
        status = buffer_append_cstr(&metadata,
                                    "\nquery_digest=", error);
    }
    if (status == QL_STATUS_OK) {
        status = buffer_append_cstr(&metadata, digest, error);
    }
    if (status == QL_STATUS_OK) {
        status = buffer_append_cstr(&metadata,
                                    "\nproof=unavailable\nexit_status=",
                                    error);
    }
    if (status == QL_STATUS_OK) {
        status = buffer_append(&metadata, status_text, (size_t)count, error);
    }
    if (status == QL_STATUS_OK) {
        status = buffer_append_cstr(&metadata, "\n", error);
    }
    if (status == QL_STATUS_OK) {
        status = create_text_artifact(
            &state->allocator, QL_ARTIFACT_KIND_SOLVER_UNSAT_METADATA,
            metadata.data, metadata.size, output, error);
    }
    buffer_dispose(&metadata);
    return status;
}

static ql_status create_diagnostics(
    const ql_bitwuzla_state *state, const char *stdout_extra,
    size_t stdout_extra_size, const ql_buffer *stderr_text,
    ql_artifact **output, ql_error *error) {
    ql_buffer diagnostics;
    ql_status status = QL_STATUS_OK;

    *output = NULL;
    if (stdout_extra_size == 0u && stderr_text->size == 0u) {
        return QL_STATUS_OK;
    }
    buffer_init(&diagnostics, &state->allocator);
    if (stdout_extra_size != 0u) {
        status = buffer_append_cstr(&diagnostics, "stdout-extra:\n", error);
        if (status == QL_STATUS_OK) {
            status = buffer_append(&diagnostics, stdout_extra,
                                   stdout_extra_size, error);
        }
        if (status == QL_STATUS_OK &&
            stdout_extra[stdout_extra_size - 1u] != '\n') {
            status = buffer_append_cstr(&diagnostics, "\n", error);
        }
    }
    if (status == QL_STATUS_OK && stderr_text->size != 0u) {
        status = buffer_append_cstr(&diagnostics, "stderr:\n", error);
        if (status == QL_STATUS_OK) {
            status = buffer_append(&diagnostics, stderr_text->data,
                                   stderr_text->size, error);
        }
        if (status == QL_STATUS_OK &&
            stderr_text->data[stderr_text->size - 1u] != '\n') {
            status = buffer_append_cstr(&diagnostics, "\n", error);
        }
    }
    if (status == QL_STATUS_OK) {
        status = create_text_artifact(
            &state->allocator, QL_ARTIFACT_KIND_SOLVER_DIAGNOSTICS,
            diagnostics.data, diagnostics.size, output, error);
    }
    buffer_dispose(&diagnostics);
    return status;
}

static ql_status parse_result_line(
    const ql_buffer *stdout_text, ql_solver_check_kind *kind,
    size_t *remainder_offset, ql_error *error) {
    size_t line_size = 0u;
    size_t remainder;

    while (line_size < stdout_text->size &&
           stdout_text->data[line_size] != '\n') {
        ++line_size;
    }
    remainder = line_size < stdout_text->size ? line_size + 1u : line_size;
    if (line_size != 0u && stdout_text->data[line_size - 1u] == '\r') {
        --line_size;
    }
    if (line_size == 3u &&
        memcmp(stdout_text->data, "sat", 3u) == 0) {
        *kind = QL_SOLVER_CHECK_SAT;
    } else if (line_size == 5u &&
               memcmp(stdout_text->data, "unsat", 5u) == 0) {
        *kind = QL_SOLVER_CHECK_UNSAT;
    } else if (line_size == 7u &&
               memcmp(stdout_text->data, "unknown", 7u) == 0) {
        *kind = QL_SOLVER_CHECK_UNKNOWN;
    } else {
        ql_error_set(error, QL_STATUS_METHOD_ERROR,
                     "Bitwuzla stdout does not begin with an exact sat, unsat, or unknown line");
        return QL_STATUS_METHOD_ERROR;
    }
    *remainder_offset = remainder;
    ql_error_clear(error);
    return QL_STATUS_OK;
}

static ql_status bitwuzla_process_failure(
    const ql_process_output *output, ql_error *error) {
    const char *diagnostic = output->stderr_text.data;
    size_t diagnostic_size = output->stderr_text.size;
    size_t copy_size = diagnostic_size;
    char message[384];

    if (copy_size >= sizeof(message)) {
        copy_size = sizeof(message) - 1u;
    }
    if (diagnostic != NULL && copy_size != 0u) {
        memcpy(message, diagnostic, copy_size);
        message[copy_size] = '\0';
        ql_error_set(error, QL_STATUS_PARSE_ERROR,
                     "Bitwuzla rejected the SMT-LIB input: %s", message);
        return QL_STATUS_PARSE_ERROR;
    }
    ql_error_set(error, QL_STATUS_METHOD_ERROR,
                 "Bitwuzla exited with status %lld and signal %d",
                 (long long)output->exit_status, output->term_signal);
    return QL_STATUS_METHOD_ERROR;
}

static ql_status QL_CALL bitwuzla_check(
    void *backend_state, const ql_solver_check_request_v1 *request,
    ql_solver_check_result_v1 *result, ql_error *error) {
    ql_bitwuzla_state *state = (ql_bitwuzla_state *)backend_state;
    ql_buffer query;
    ql_process_output output;
    char timeout_text[32];
    char memory_limit_text[32];
    char *arguments[13];
    size_t argument_count = 0u;
    size_t result_remainder = 0u;
    const char *stdout_extra = NULL;
    size_t stdout_extra_size = 0u;
    int timeout_count = 0;
    int wants_model =
        (request->artifact_requests & QL_SOLVER_REQUEST_MODEL) != 0u;
    ql_status status;
    ql_status integrity_status;
    ql_error process_error;

    buffer_init(&query, &state->allocator);
    result->backend_binary_digest = state->executable_digest;
    status = buffer_append_limited(
        &query, state->transcript.data, state->transcript.size,
        QL_SOLVER_MAX_SMTLIB2_BYTES, "Bitwuzla SMT-LIB query", error);
    if (status == QL_STATUS_OK) {
        status = buffer_append_limited(
            &query, "(check-sat)\n", strlen("(check-sat)\n"),
            QL_SOLVER_MAX_SMTLIB2_BYTES, "Bitwuzla SMT-LIB query", error);
    }
    if (status != QL_STATUS_OK) {
        buffer_dispose(&query);
        return status;
    }
    ql_digest_data(query.data, query.size, &result->query_digest);
    if (request->is_cancelled != NULL &&
        request->is_cancelled(request->cancel_state) != 0u) {
        buffer_dispose(&query);
        result->kind = QL_SOLVER_CHECK_UNKNOWN;
        result->unknown_reason = QL_SOLVER_UNKNOWN_CANCELLED;
        ql_error_clear(error);
        return QL_STATUS_OK;
    }
    if (wants_model) {
        status = buffer_append_limited(
            &query, "(get-model)\n", strlen("(get-model)\n"),
            QL_SOLVER_MAX_SMTLIB2_BYTES, "Bitwuzla SMT-LIB query", error);
    }
    if (status == QL_STATUS_OK) {
        status = buffer_append_limited(
            &query, "(exit)\n", strlen("(exit)\n"),
            QL_SOLVER_MAX_SMTLIB2_BYTES, "Bitwuzla SMT-LIB query", error);
    }
    if (status != QL_STATUS_OK) {
        buffer_dispose(&query);
        return status;
    }
    if (query.size > UINT_MAX) {
        buffer_dispose(&query);
        ql_error_set(error, QL_STATUS_INVALID_ARGUMENT,
                     "SMT-LIB query exceeds the process transport limit");
        return QL_STATUS_INVALID_ARGUMENT;
    }

    arguments[argument_count++] = state->executable;
    arguments[argument_count++] = (char *)"--lang";
    arguments[argument_count++] = (char *)"smt2";
    arguments[argument_count++] = (char *)"--bv-output-format";
    arguments[argument_count++] = (char *)"16";
    if (wants_model) {
        arguments[argument_count++] = (char *)"--produce-models";
    }
    if (request->timeout_ms != 0u) {
        timeout_count = snprintf(timeout_text, sizeof(timeout_text), "%llu",
                                 (unsigned long long)request->timeout_ms);
        if (timeout_count <= 0 ||
            (size_t)timeout_count >= sizeof(timeout_text)) {
            buffer_dispose(&query);
            ql_error_set(error, QL_STATUS_INTERNAL_ERROR,
                         "could not format solver timeout");
            return QL_STATUS_INTERNAL_ERROR;
        }
        arguments[argument_count++] = (char *)"--time-limit";
        arguments[argument_count++] = timeout_text;
    }
    if (request->memory_limit_mb != 0u) {
        const int memory_limit_count = snprintf(
            memory_limit_text, sizeof(memory_limit_text), "%llu",
            (unsigned long long)request->memory_limit_mb);
        if (memory_limit_count <= 0 ||
            (size_t)memory_limit_count >= sizeof(memory_limit_text)) {
            buffer_dispose(&query);
            ql_error_set(error, QL_STATUS_INTERNAL_ERROR,
                         "could not format solver memory limit");
            return QL_STATUS_INTERNAL_ERROR;
        }
        arguments[argument_count++] = (char *)"--memory-limit";
        arguments[argument_count++] = memory_limit_text;
    }
    arguments[argument_count] = NULL;

    status = verify_snapshot_digest(
        state, "before the solver check", error);
    if (status != QL_STATUS_OK) {
        buffer_dispose(&query);
        return status;
    }
    status = run_process(&state->allocator, state->executable, arguments,
                         query.data, query.size, request, &output, error);
    if (error == NULL) {
        ql_error_clear(&process_error);
    } else {
        process_error = *error;
    }
    buffer_dispose(&query);
    integrity_status = verify_snapshot_digest(
        state, "during the solver check", error);
    if (integrity_status != QL_STATUS_OK) {
        if (status == QL_STATUS_OK) {
            process_output_dispose(&output);
        }
        return integrity_status;
    }
    if (status != QL_STATUS_OK) {
        if (error != NULL) {
            *error = process_error;
        }
        return status;
    }
    if (output.cancelled || output.timed_out) {
        result->kind = QL_SOLVER_CHECK_UNKNOWN;
        result->unknown_reason = ql_solver_unknown_reason_classify(
            (uint32_t)output.cancelled, (uint32_t)output.timed_out);
        if ((request->artifact_requests & QL_SOLVER_REQUEST_DIAGNOSTICS) !=
            0u) {
            status = create_diagnostics(
                state, output.stdout_text.data, output.stdout_text.size,
                &output.stderr_text, &result->diagnostics_artifact, error);
        }
        process_output_dispose(&output);
        return status;
    }
    if (output.exit_status != 0 || output.term_signal != 0) {
        status = bitwuzla_process_failure(&output, error);
        process_output_dispose(&output);
        return status;
    }
    status = parse_result_line(&output.stdout_text, &result->kind,
                               &result_remainder, error);
    if (status != QL_STATUS_OK) {
        if (output.stderr_text.size != 0u) {
            status = bitwuzla_process_failure(&output, error);
        }
        process_output_dispose(&output);
        return status;
    }
    stdout_extra = output.stdout_text.data + result_remainder;
    stdout_extra_size = output.stdout_text.size - result_remainder;

    if (result->kind == QL_SOLVER_CHECK_SAT && wants_model) {
        if (stdout_extra_size == 0u) {
            process_output_dispose(&output);
            ql_error_set(error, QL_STATUS_METHOD_ERROR,
                         "Bitwuzla returned SAT without the requested model");
            return QL_STATUS_METHOD_ERROR;
        }
        status = create_text_artifact(
            &state->allocator, QL_ARTIFACT_KIND_SOLVER_MODEL, stdout_extra,
            stdout_extra_size, &result->model_artifact, error);
        stdout_extra = NULL;
        stdout_extra_size = 0u;
    } else if (result->kind == QL_SOLVER_CHECK_UNSAT &&
               (request->artifact_requests &
                QL_SOLVER_REQUEST_UNSAT_METADATA) != 0u) {
        status = create_unsat_metadata(state, result, output.exit_status,
                                       &result->unsat_metadata_artifact,
                                       error);
    } else if (result->kind == QL_SOLVER_CHECK_UNKNOWN) {
        result->unknown_reason = ql_solver_unknown_reason_classify(0u, 0u);
    }
    if (status == QL_STATUS_OK &&
        (request->artifact_requests & QL_SOLVER_REQUEST_DIAGNOSTICS) != 0u) {
        status = create_diagnostics(
            state, stdout_extra, stdout_extra_size, &output.stderr_text,
            &result->diagnostics_artifact, error);
    }
    process_output_dispose(&output);
    return status;
}

static const ql_solver_descriptor_v1 bitwuzla_descriptor = {
    sizeof(ql_solver_descriptor_v1),
    QL_SOLVER_ABI_VERSION,
    0u,
    "bitwuzla",
    QL_BITWUZLA_VERSION,
    "Pinned Bitwuzla process adapter",
    {
        sizeof(ql_solver_capability_v1),
        QL_SOLVER_ABI_VERSION,
        QL_BITWUZLA_AVAILABILITY,
        QL_SOLVER_LOGIC_ALL,
        1u,
        UINT32_MAX,
        QL_SOLVER_FEATURE_ARRAYS |
            QL_SOLVER_FEATURE_FLOATING_POINT |
            QL_SOLVER_FEATURE_INCREMENTAL |
            QL_SOLVER_FEATURE_MODELS |
            QL_SOLVER_FEATURE_CANCELLATION |
            QL_SOLVER_FEATURE_TIMEOUT |
            QL_SOLVER_FEATURE_PROCESS_ISOLATION |
            QL_SOLVER_FEATURE_MEMORY_LIMIT,
        {0u, 0u, 0u, 0u, 0u, 0u},
    },
    bitwuzla_create,
    bitwuzla_destroy,
    bitwuzla_add_smt2,
    bitwuzla_push,
    bitwuzla_pop,
    bitwuzla_check,
    {NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL},
};

const ql_solver_descriptor_v1 *QL_CALL
ql_bitwuzla_solver_descriptor(void) {
    return &bitwuzla_descriptor;
}

const char *QL_CALL ql_bitwuzla_executable_path(void) {
    return QL_BITWUZLA_EXECUTABLE;
}
