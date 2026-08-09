#include "quodlibet/quodlibet.h"

#include "coverage.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void usage(FILE *stream) {
    (void)fprintf(stream,
                  "usage:\n"
                  "  quodlibet version\n"
                  "  quodlibet methods\n"
                  "  quodlibet parse-c <source.c>\n"
                  "  quodlibet validate <pipeline.json>\n"
                  "  quodlibet coverage <units.txt> [detail.tsv]\n");
}

static char *read_file(const char *path, size_t *size, ql_error *error) {
    FILE *file;
    long file_size;
    char *data;
    size_t read_size;

    *size = 0u;
#if defined(_WIN32)
    if (fopen_s(&file, path, "rb") != 0) {
        file = NULL;
    }
#else
    file = fopen(path, "rb");
#endif
    if (file == NULL) {
        ql_error_set(error, QL_STATUS_IO_ERROR, "cannot open '%s'", path);
        return NULL;
    }
    if (fseek(file, 0L, SEEK_END) != 0 || (file_size = ftell(file)) < 0L ||
        fseek(file, 0L, SEEK_SET) != 0) {
        (void)fclose(file);
        ql_error_set(error, QL_STATUS_IO_ERROR, "cannot size '%s'", path);
        return NULL;
    }
    data = malloc((size_t)file_size + 1u);
    if (data == NULL) {
        (void)fclose(file);
        ql_error_set(error, QL_STATUS_OUT_OF_MEMORY, NULL);
        return NULL;
    }
    read_size = fread(data, 1u, (size_t)file_size, file);
    if (read_size != (size_t)file_size || fclose(file) != 0) {
        free(data);
        ql_error_set(error, QL_STATUS_IO_ERROR, "cannot read '%s'", path);
        return NULL;
    }
    data[read_size] = '\0';
    *size = read_size;
    return data;
}

static int print_error(const ql_error *error) {
    (void)fprintf(stderr, "error: %s\n",
                  error != NULL && error->message[0] != '\0'
                      ? error->message
                      : "operation failed");
    return 1;
}

static int find_c_recovery_node(ql_c_syntax_tree *tree,
                                ql_c_syntax_node_view *recovery,
                                ql_error *error) {
    ql_c_syntax_cursor *cursor = NULL;
    int result = 0;

    if (ql_c_syntax_cursor_create(tree, &cursor, error) != QL_STATUS_OK) {
        return -1;
    }
    for (;;) {
        ql_c_syntax_node_view view = { 0 };
        view.struct_size = sizeof(view);
        if (ql_c_syntax_cursor_current(cursor, &view, error) !=
            QL_STATUS_OK) {
            result = -1;
            break;
        }
        if ((view.flags & (QL_C_SYNTAX_NODE_ERROR |
                           QL_C_SYNTAX_NODE_MISSING)) != 0u) {
            *recovery = view;
            result = 1;
            break;
        }
        if (ql_c_syntax_cursor_goto_first_child(cursor) != 0u) {
            continue;
        }
        while (ql_c_syntax_cursor_goto_next_sibling(cursor) == 0u) {
            if (ql_c_syntax_cursor_goto_parent(cursor) == 0u) {
                goto done;
            }
        }
    }

done:
    ql_c_syntax_cursor_destroy(cursor);
    return result;
}

static int parse_c_file(const char *path, ql_error *error) {
    ql_c_parser *parser = NULL;
    ql_c_syntax_tree *tree = NULL;
    ql_c_syntax_node_view recovery = { 0 };
    char *source = NULL;
    size_t source_size = 0u;
    ql_status status;
    int found;
    int result = 0;

    source = read_file(path, &source_size, error);
    if (source == NULL) {
        return print_error(error);
    }
    status = ql_c_parser_create(NULL, &parser, error);
    if (status != QL_STATUS_OK) {
        result = print_error(error);
        goto cleanup;
    }
    status = ql_c_parser_parse(parser, source, source_size, &tree, error);
    if (status != QL_STATUS_OK) {
        result = print_error(error);
        goto cleanup;
    }
    if (ql_c_syntax_tree_has_errors(tree) == 0u) {
        (void)printf("valid C syntax: %s\n", path);
        goto cleanup;
    }

    recovery.struct_size = sizeof(recovery);
    found = find_c_recovery_node(tree, &recovery, error);
    if (found < 0) {
        result = print_error(error);
        goto cleanup;
    }
    if (found != 0) {
        const char *description =
            (recovery.flags & QL_C_SYNTAX_NODE_MISSING) != 0u
                ? "missing"
                : "unexpected";
        (void)fprintf(stderr, "%s:%llu:%llu: syntax error: %s %s\n", path,
                      (unsigned long long)recovery.range.start_point.row + 1u,
                      (unsigned long long)recovery.range.start_point.column +
                          1u,
                      description, recovery.kind);
    } else {
        (void)fprintf(stderr, "%s: syntax error\n", path);
    }
    result = 1;

cleanup:
    ql_c_syntax_tree_destroy(tree);
    ql_c_parser_destroy(parser);
    free(source);
    return result;
}

int main(int argc, char **argv) {
    ql_registry *registry = NULL;
    ql_pipeline *pipeline = NULL;
    ql_error error;
    ql_status status;
    char *json = NULL;
    size_t json_size = 0u;
    int result = 0;

    if (argc == 2 && strcmp(argv[1], "version") == 0) {
        (void)printf("quodlibet %s (ABI %u, pipeline schema %u)\n",
                     ql_version_string(), QL_ABI_VERSION,
                     QL_PIPELINE_SCHEMA_VERSION);
        return 0;
    }
    if (argc >= 3 && strcmp(argv[1], "coverage") == 0 && argc <= 4) {
        return ql_cli_coverage(argv[2], argc == 4 ? argv[3] : NULL);
    }
    if (argc != 2 && argc != 3) {
        usage(stderr);
        return 2;
    }
    ql_error_clear(&error);
    if (argc == 3 && strcmp(argv[1], "parse-c") == 0) {
        return parse_c_file(argv[2], &error);
    }
    status = ql_registry_create(NULL, &registry, &error);
    if (status != QL_STATUS_OK) {
        return print_error(&error);
    }
    status = ql_register_builtin_methods(registry, &error);
    if (status != QL_STATUS_OK) {
        result = print_error(&error);
        goto cleanup;
    }
    if (argc == 2 && strcmp(argv[1], "methods") == 0) {
        size_t index;
        for (index = 0u; index < ql_registry_count(registry); ++index) {
            const ql_method_v1 *method = ql_registry_at(registry, index);
            (void)printf("%s\t%s\n", method->name,
                         method->description != NULL ? method->description : "");
        }
        goto cleanup;
    }
    if (argc == 3 && strcmp(argv[1], "validate") == 0) {
        json = read_file(argv[2], &json_size, &error);
        if (json == NULL) {
            result = print_error(&error);
            goto cleanup;
        }
        status = ql_pipeline_from_json(registry, NULL, json, json_size,
                                       &pipeline, &error);
        if (status != QL_STATUS_OK) {
            result = print_error(&error);
            goto cleanup;
        }
        (void)printf("valid pipeline: %s\n", argv[2]);
        goto cleanup;
    }
    usage(stderr);
    result = 2;

cleanup:
    free(json);
    ql_pipeline_destroy(pipeline);
    ql_registry_destroy(registry);
    return result;
}
