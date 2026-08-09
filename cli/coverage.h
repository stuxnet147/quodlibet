#ifndef QUODLIBET_CLI_COVERAGE_H
#define QUODLIBET_CLI_COVERAGE_H

#include "quodlibet/common.h"

QL_EXTERN_C_BEGIN

/* Measure how much of a corpus reaches a proof IR. `list_path` is a file of
   newline-separated translation-unit paths. `detail_path` may be NULL; when
   given it receives one tab-separated row per function definition. A JSON
   summary is written to stdout. */
int ql_cli_coverage(const char *list_path, const char *detail_path);

QL_EXTERN_C_END

#endif
