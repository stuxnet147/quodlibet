#ifndef QUODLIBET_VERSION_H
#define QUODLIBET_VERSION_H

#include "quodlibet/common.h"

#define QL_VERSION_MAJOR 0
#define QL_VERSION_MINOR 1
#define QL_VERSION_PATCH 0
#define QL_VERSION_STRING "0.1.0"

QL_EXTERN_C_BEGIN

QL_API const char *QL_CALL ql_version_string(void);

QL_EXTERN_C_END

#endif
