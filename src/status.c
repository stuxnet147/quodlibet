#include "quodlibet/status.h"

#include <stdarg.h>
#include <stdio.h>

const char *QL_CALL ql_status_string(ql_status status) {
    switch (status) {
    case QL_STATUS_OK: return "ok";
    case QL_STATUS_INVALID_ARGUMENT: return "invalid argument";
    case QL_STATUS_OUT_OF_MEMORY: return "out of memory";
    case QL_STATUS_NOT_FOUND: return "not found";
    case QL_STATUS_ALREADY_EXISTS: return "already exists";
    case QL_STATUS_ABI_MISMATCH: return "ABI mismatch";
    case QL_STATUS_SCHEMA_MISMATCH: return "schema mismatch";
    case QL_STATUS_TYPE_MISMATCH: return "type mismatch";
    case QL_STATUS_CYCLE: return "cycle";
    case QL_STATUS_CANCELLED: return "cancelled";
    case QL_STATUS_IO_ERROR: return "I/O error";
    case QL_STATUS_PARSE_ERROR: return "parse error";
    case QL_STATUS_PLUGIN_ERROR: return "plugin error";
    case QL_STATUS_METHOD_ERROR: return "method error";
    case QL_STATUS_INTERNAL_ERROR: return "internal error";
    default: return "unknown status";
    }
}

void QL_CALL ql_error_clear(ql_error *error) {
    if (error == NULL) {
        return;
    }
    error->code = QL_STATUS_OK;
    error->message[0] = '\0';
}

void QL_CALL ql_error_set(ql_error *error, ql_status code,
                          const char *format, ...) {
    va_list arguments;

    if (error == NULL) {
        return;
    }
    error->code = code;
    if (format == NULL) {
        (void)snprintf(error->message, sizeof(error->message), "%s",
                       ql_status_string(code));
        return;
    }
    va_start(arguments, format);
    (void)vsnprintf(error->message, sizeof(error->message), format, arguments);
    va_end(arguments);
    error->message[sizeof(error->message) - 1u] = '\0';
}
