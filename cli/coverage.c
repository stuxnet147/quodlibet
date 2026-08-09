#include "coverage.h"

#include "quodlibet/c_frontend.h"
#include "quodlibet/c_lower.h"
#include "quodlibet/c_syntax.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if !defined(_WIN32)
#  include <fcntl.h>
#  include <sys/stat.h>
#  include <sys/types.h>
#  include <unistd.h>
#endif

/* Coverage answers one question: what share of a real corpus reaches a proof
   IR, and what stops the rest. It is a measurement entry point, not a proof
   path, so it never reports a verdict and never treats an accepted parse as
   semantic support. */

#define QL_COVERAGE_FRONTEND_CODE_MAX 8u
#define QL_COVERAGE_LOWER_CODE_MAX 16u

static const char *const k_frontend_code_names[QL_COVERAGE_FRONTEND_CODE_MAX] = {
    "none",
    "preprocessing_required",
    "variadic_function",
    "old_style_function",
    "unsupported_construct",
    "unsupported_signature",
    "unnamed_parameter",
    "duplicate_definition"
};

static const char *const k_lower_code_names[QL_COVERAGE_LOWER_CODE_MAX] = {
    "none",
    "frontend_unsupported",
    "unsupported_type",
    "unsupported_pointer",
    "unsupported_call",
    "unsupported_loop",
    "unsupported_volatile_or_atomic",
    "unsupported_control_flow",
    "unsupported_expression",
    "invalid_declaration",
    "undeclared_identifier",
    "uninitialized_read",
    "duplicate_declaration",
    "type_error",
    "missing_return",
    "integer_literal_out_of_range"
};

typedef struct ql_coverage_totals {
    unsigned long long units;
    unsigned long long units_unreadable;
    unsigned long long units_syntax_error;
    unsigned long long definitions;
    unsigned long long definitions_frontend_supported;
    unsigned long long definitions_lowered;
    unsigned long long definitions_lower_failed_status;
    unsigned long long frontend_codes[QL_COVERAGE_FRONTEND_CODE_MAX];
    unsigned long long lower_codes[QL_COVERAGE_LOWER_CODE_MAX];
} ql_coverage_totals;

/* Sizing the unit by seeking to its end and back costs two extra lseek
   syscalls per file. Over a corpus that is the whole of this function's
   profile: fseek alone was 2.9% of the tool's CPU on the val split and this
   tool runs over 188,432 records on the train split. Ask the descriptor for
   the size instead. The buffered-stream fallback stays for platforms without
   the POSIX pair. */
static char *coverage_read_file(const char *path, size_t *size) {
#if !defined(_WIN32)
    int descriptor;
    struct stat info;
    char *data;
    size_t total = 0u;

    *size = 0u;
    descriptor = open(path, O_RDONLY);
    if (descriptor < 0) {
        return NULL;
    }
    if (fstat(descriptor, &info) != 0 || !S_ISREG(info.st_mode) ||
        info.st_size < 0) {
        (void)close(descriptor);
        return NULL;
    }
    data = malloc((size_t)info.st_size + 1u);
    if (data == NULL) {
        (void)close(descriptor);
        return NULL;
    }
    while (total < (size_t)info.st_size) {
        ssize_t chunk = read(descriptor, data + total,
                             (size_t)info.st_size - total);
        if (chunk <= 0) {
            /* A short read means the file changed under the measurement.
               Refusing it keeps the denominator honest. */
            free(data);
            (void)close(descriptor);
            return NULL;
        }
        total += (size_t)chunk;
    }
    (void)close(descriptor);
    data[total] = '\0';
    *size = total;
    return data;
#else
    FILE *file;
    long file_size;
    char *data;
    size_t read_size;

    *size = 0u;
    if (fopen_s(&file, path, "rb") != 0) {
        file = NULL;
    }
    if (file == NULL) {
        return NULL;
    }
    if (fseek(file, 0L, SEEK_END) != 0 || (file_size = ftell(file)) < 0L ||
        fseek(file, 0L, SEEK_SET) != 0) {
        (void)fclose(file);
        return NULL;
    }
    data = malloc((size_t)file_size + 1u);
    if (data == NULL) {
        (void)fclose(file);
        return NULL;
    }
    read_size = fread(data, 1u, (size_t)file_size, file);
    if (read_size != (size_t)file_size || fclose(file) != 0) {
        free(data);
        return NULL;
    }
    data[read_size] = '\0';
    *size = read_size;
    return data;
#endif
}

static int coverage_has_body(const ql_c_function_view *view) {
    return view->body_range.end_byte > view->body_range.start_byte;
}

static void coverage_count_frontend(ql_coverage_totals *totals,
                                    const ql_c_frontend_unit *unit,
                                    size_t function_index,
                                    size_t diagnostic_count) {
    size_t index;
    ql_error error;

    for (index = 0u; index < diagnostic_count; ++index) {
        ql_c_frontend_diagnostic_view view = { 0 };
        view.struct_size = sizeof(view);
        if (ql_c_frontend_function_diagnostic_at(unit, function_index, index,
                                                 &view, &error) !=
            QL_STATUS_OK) {
            break;
        }
        if ((unsigned)view.code < QL_COVERAGE_FRONTEND_CODE_MAX) {
            totals->frontend_codes[(unsigned)view.code] += 1u;
        }
    }
}

static void coverage_count_lower(ql_coverage_totals *totals,
                                 const ql_c_lower_result *result,
                                 size_t diagnostic_count) {
    size_t index;
    ql_error error;

    for (index = 0u; index < diagnostic_count; ++index) {
        ql_c_lower_diagnostic_view_v1 view = { 0 };
        view.struct_size = sizeof(view);
        if (ql_c_lower_result_diagnostic_at(result, index, &view, &error) !=
            QL_STATUS_OK) {
            break;
        }
        if ((unsigned)view.code < QL_COVERAGE_LOWER_CODE_MAX) {
            totals->lower_codes[(unsigned)view.code] += 1u;
        }
    }
}

static void coverage_measure_unit(ql_coverage_totals *totals,
                                  const char *path, FILE *detail) {
    char *source = NULL;
    size_t source_size = 0u;
    ql_c_frontend_unit *unit = NULL;
    ql_c_frontend_unit_view unit_view = { 0 };
    ql_error error;
    ql_status status;
    size_t index;

    totals->units += 1u;
    source = coverage_read_file(path, &source_size);
    if (source == NULL) {
        totals->units_unreadable += 1u;
        return;
    }

    /* The frontend parses internally and reports a recovered-error tree as
       QL_STATUS_PARSE_ERROR, so a separate syntax-check parse here would just
       parse every unit twice. VTune showed parsing dominating this tool. */
    ql_error_clear(&error);
    status = ql_c_frontend_analyze(NULL, source, source_size, &unit, &error);
    if (status == QL_STATUS_PARSE_ERROR) {
        totals->units_syntax_error += 1u;
        free(source);
        return;
    }
    if (status != QL_STATUS_OK) {
        totals->units_unreadable += 1u;
        free(source);
        return;
    }
    unit_view.struct_size = sizeof(unit_view);
    if (ql_c_frontend_unit_get_view(unit, &unit_view, &error) !=
        QL_STATUS_OK) {
        totals->units_unreadable += 1u;
        ql_c_frontend_unit_destroy(unit);
        free(source);
        return;
    }

    for (index = 0u; index < unit_view.function_count; ++index) {
        ql_c_function_view function = { 0 };
        ql_c_lower_result *lowered = NULL;
        ql_c_lower_result_view_v1 lower_view = { 0 };
        const char *outcome = "lower_error";
        unsigned first_lower_code = 0u;

        function.struct_size = sizeof(function);
        if (ql_c_frontend_function_at(unit, index, &function, &error) !=
            QL_STATUS_OK) {
            continue;
        }
        /* Context declarations are prototypes. Only a definition can lower. */
        if (coverage_has_body(&function) == 0) {
            continue;
        }
        totals->definitions += 1u;

        if (function.support == QL_C_FUNCTION_SUPPORTED) {
            totals->definitions_frontend_supported += 1u;
        } else {
            coverage_count_frontend(totals, unit, index,
                                    function.diagnostic_count);
        }

        if (ql_c_lower_selected_function(NULL, source, source_size, unit,
                                         &function, &lowered,
                                         &error) != QL_STATUS_OK) {
            totals->definitions_lower_failed_status += 1u;
        } else {
            lower_view.struct_size = sizeof(lower_view);
            if (ql_c_lower_result_get_view(lowered, &lower_view, &error) !=
                QL_STATUS_OK) {
                totals->definitions_lower_failed_status += 1u;
            } else if (lower_view.support == QL_C_LOWER_SUPPORTED) {
                totals->definitions_lowered += 1u;
                outcome = "lowered";
            } else {
                outcome = "unknown";
                coverage_count_lower(totals, lowered,
                                     lower_view.diagnostic_count);
                if (lower_view.diagnostic_count > 0u) {
                    ql_c_lower_diagnostic_view_v1 first = { 0 };
                    first.struct_size = sizeof(first);
                    if (ql_c_lower_result_diagnostic_at(lowered, 0u, &first,
                                                        &error) ==
                        QL_STATUS_OK) {
                        first_lower_code = (unsigned)first.code;
                    }
                }
            }
        }
        ql_c_lower_result_destroy(lowered);

        if (detail != NULL) {
            const char *code_name =
                first_lower_code < QL_COVERAGE_LOWER_CODE_MAX
                    ? k_lower_code_names[first_lower_code]
                    : "unknown_code";
            (void)fprintf(detail, "%s\t%s\t%s\t%s\n", path,
                          function.name != NULL ? function.name : "?", outcome,
                          code_name);
        }
    }

    ql_c_frontend_unit_destroy(unit);
    free(source);
}

static void coverage_print_json(const ql_coverage_totals *totals) {
    unsigned index;
    int printed;

    (void)printf("{\n");
    (void)printf("  \"schema_version\": 1,\n");
    (void)printf("  \"units\": %llu,\n", totals->units);
    (void)printf("  \"units_unreadable\": %llu,\n", totals->units_unreadable);
    (void)printf("  \"units_syntax_error\": %llu,\n",
                 totals->units_syntax_error);
    (void)printf("  \"definitions\": %llu,\n", totals->definitions);
    (void)printf("  \"definitions_frontend_supported\": %llu,\n",
                 totals->definitions_frontend_supported);
    (void)printf("  \"definitions_lowered\": %llu,\n",
                 totals->definitions_lowered);
    (void)printf("  \"definitions_lower_failed_status\": %llu,\n",
                 totals->definitions_lower_failed_status);

    printed = 0;
    (void)printf("  \"frontend_diagnostics\": {");
    for (index = 1u; index < QL_COVERAGE_FRONTEND_CODE_MAX; ++index) {
        if (totals->frontend_codes[index] == 0u) {
            continue;
        }
        (void)printf("%s\n    \"%s\": %llu", printed != 0 ? "," : "",
                     k_frontend_code_names[index], totals->frontend_codes[index]);
        printed = 1;
    }
    (void)printf("%s},\n", printed != 0 ? "\n  " : "");

    printed = 0;
    (void)printf("  \"lower_diagnostics\": {");
    for (index = 1u; index < QL_COVERAGE_LOWER_CODE_MAX; ++index) {
        if (totals->lower_codes[index] == 0u) {
            continue;
        }
        (void)printf("%s\n    \"%s\": %llu", printed != 0 ? "," : "",
                     k_lower_code_names[index], totals->lower_codes[index]);
        printed = 1;
    }
    (void)printf("%s}\n", printed != 0 ? "\n  " : "");
    (void)printf("}\n");
}

int ql_cli_coverage(const char *list_path, const char *detail_path) {
    ql_coverage_totals totals;
    ql_error error;
    FILE *list = NULL;
    FILE *detail = NULL;
    char line[4096];

    memset(&totals, 0, sizeof(totals));
    ql_error_clear(&error);

#if defined(_WIN32)
    if (fopen_s(&list, list_path, "rb") != 0) {
        list = NULL;
    }
#else
    list = fopen(list_path, "rb");
#endif
    if (list == NULL) {
        (void)fprintf(stderr, "error: cannot open list '%s'\n", list_path);
        return 1;
    }
    if (detail_path != NULL) {
#if defined(_WIN32)
        if (fopen_s(&detail, detail_path, "wb") != 0) {
            detail = NULL;
        }
#else
        detail = fopen(detail_path, "wb");
#endif
        if (detail == NULL) {
            (void)fclose(list);
            (void)fprintf(stderr, "error: cannot write detail '%s'\n",
                          detail_path);
            return 1;
        }
    }
    while (fgets(line, (int)sizeof(line), list) != NULL) {
        size_t length = strlen(line);
        while (length > 0u &&
               (line[length - 1u] == '\n' || line[length - 1u] == '\r')) {
            line[--length] = '\0';
        }
        if (length == 0u) {
            continue;
        }
        coverage_measure_unit(&totals, line, detail);
    }

    (void)fclose(list);
    if (detail != NULL) {
        (void)fclose(detail);
    }
    coverage_print_json(&totals);
    return 0;
}
