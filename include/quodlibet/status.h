#ifndef QUODLIBET_STATUS_H
#define QUODLIBET_STATUS_H

#include "quodlibet/common.h"

#define QL_ERROR_MESSAGE_CAPACITY 512u

QL_EXTERN_C_BEGIN

typedef enum ql_status {
    QL_STATUS_OK = 0,
    QL_STATUS_INVALID_ARGUMENT,
    QL_STATUS_OUT_OF_MEMORY,
    QL_STATUS_NOT_FOUND,
    QL_STATUS_ALREADY_EXISTS,
    QL_STATUS_ABI_MISMATCH,
    QL_STATUS_SCHEMA_MISMATCH,
    QL_STATUS_TYPE_MISMATCH,
    QL_STATUS_CYCLE,
    QL_STATUS_CANCELLED,
    QL_STATUS_IO_ERROR,
    QL_STATUS_PARSE_ERROR,
    QL_STATUS_PLUGIN_ERROR,
    QL_STATUS_METHOD_ERROR,
    QL_STATUS_INTERNAL_ERROR
} ql_status;

typedef struct ql_error {
    ql_status code;
    char message[QL_ERROR_MESSAGE_CAPACITY];
} ql_error;

QL_API const char *QL_CALL ql_status_string(ql_status status);
QL_API void QL_CALL ql_error_clear(ql_error *error);
QL_API void QL_CALL ql_error_set(ql_error *error, ql_status code,
                                 const char *format, ...);

QL_EXTERN_C_END

#endif
