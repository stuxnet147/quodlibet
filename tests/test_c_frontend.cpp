#include "quodlibet/c_frontend.h"

#include <cstddef>
#include <cstring>

#include <gtest/gtest.h>

namespace {

class CFrontendUnit {
public:
    ~CFrontendUnit() { ql_c_frontend_unit_destroy(unit); }

    ql_c_frontend_unit **output() { return &unit; }
    ql_c_frontend_unit *get() const { return unit; }

private:
    ql_c_frontend_unit *unit = nullptr;
};

ql_c_frontend_diagnostic_view FindFunctionDiagnostic(
    const ql_c_frontend_unit *unit, std::size_t function_index,
    ql_c_frontend_diagnostic_code code) {
    for (std::size_t index = 0u;; ++index) {
        ql_c_frontend_diagnostic_view diagnostic{};
        ql_error error{};
        diagnostic.struct_size = sizeof(diagnostic);
        if (ql_c_frontend_function_diagnostic_at(
                unit, function_index, index, &diagnostic, &error) !=
            QL_STATUS_OK) {
            return {};
        }
        if (diagnostic.code == code) {
            return diagnostic;
        }
    }
}

TEST(CFrontend, InventoriesDefinitionsTypesAndSelectsByName) {
    constexpr char source[] =
        "typedef unsigned long word;\n"
        "static const word *lookup(const word *items, unsigned count) {\n"
        "  return count != 0 ? items : 0;\n"
        "}\n"
        "void reset(void) {}\n";
    CFrontendUnit analyzed;
    ql_c_frontend_unit_view unit{};
    ql_c_function_view function{};
    ql_c_parameter_view first{};
    ql_c_parameter_view second{};
    ql_error error{};

    ASSERT_EQ(QL_STATUS_OK,
              ql_c_frontend_analyze(nullptr, source, sizeof(source) - 1u,
                                    analyzed.output(), &error))
        << error.message;
    unit.struct_size = sizeof(unit);
    ASSERT_EQ(QL_STATUS_OK,
              ql_c_frontend_unit_get_view(analyzed.get(), &unit, &error));
    EXPECT_EQ(QL_C_FRONTEND_SCHEMA_VERSION, unit.schema_version);
    EXPECT_EQ(QL_C_FUNCTION_SUPPORTED, unit.support);
    EXPECT_EQ(2u, unit.function_count);
    EXPECT_EQ(0u, unit.diagnostic_count);

    function.struct_size = sizeof(function);
    ASSERT_EQ(QL_STATUS_OK,
              ql_c_frontend_select_function(analyzed.get(), "lookup", 6u,
                                            &function, &error))
        << error.message;
    EXPECT_STREQ("lookup", function.name);
    EXPECT_STREQ("static const word *lookup(const word *items, unsigned count)",
                 function.signature_spelling);
    EXPECT_EQ(QL_C_FUNCTION_SUPPORTED, function.support);
    EXPECT_EQ(2u, function.parameter_count);
    EXPECT_EQ(QL_C_TYPE_BASE_TYPEDEF_NAME,
              function.return_type.base_kind);
    EXPECT_EQ(1u, function.return_type.pointer_depth);
    EXPECT_NE(0u, function.return_type.shape & QL_C_TYPE_SHAPE_POINTER);
    EXPECT_NE(0u,
              function.return_type.qualifiers & QL_C_TYPE_QUALIFIER_CONST);
    EXPECT_STREQ("word", function.return_type.base_spelling);
    EXPECT_STREQ("*lookup", function.return_type.declarator_spelling);

    first.struct_size = sizeof(first);
    ASSERT_EQ(QL_STATUS_OK,
              ql_c_frontend_parameter_at(analyzed.get(), function.index, 0u,
                                         &first, &error));
    EXPECT_STREQ("items", first.name);
    EXPECT_EQ(QL_C_TYPE_BASE_TYPEDEF_NAME, first.type.base_kind);
    EXPECT_EQ(1u, first.type.pointer_depth);
    EXPECT_NE(0u, first.type.qualifiers & QL_C_TYPE_QUALIFIER_CONST);

    second.struct_size = sizeof(second);
    ASSERT_EQ(QL_STATUS_OK,
              ql_c_frontend_parameter_at(analyzed.get(), function.index, 1u,
                                         &second, &error));
    EXPECT_STREQ("count", second.name);
    EXPECT_EQ(QL_C_TYPE_BASE_INTEGER, second.type.base_kind);
    EXPECT_EQ(0u, second.type.pointer_depth);

    function = {};
    function.struct_size = sizeof(function);
    ASSERT_EQ(QL_STATUS_OK,
              ql_c_frontend_select_function(analyzed.get(), "reset", 5u,
                                            &function, &error));
    EXPECT_EQ(0u, function.parameter_count);
    EXPECT_EQ(QL_C_TYPE_BASE_VOID, function.return_type.base_kind);
}

TEST(CFrontend, ClassifiesVariadicDefinitionAsUnsupported) {
    constexpr char source[] =
        "int emit(const char *format, ...) { return format != 0; }";
    CFrontendUnit analyzed;
    ql_c_function_view function{};
    ql_c_frontend_diagnostic_view diagnostic{};
    ql_error error{};

    ASSERT_EQ(QL_STATUS_OK,
              ql_c_frontend_analyze(nullptr, source, sizeof(source) - 1u,
                                    analyzed.output(), &error))
        << error.message;
    function.struct_size = sizeof(function);
    ASSERT_EQ(QL_STATUS_OK,
              ql_c_frontend_function_at(analyzed.get(), 0u, &function,
                                        &error));
    EXPECT_EQ(QL_C_FUNCTION_UNSUPPORTED, function.support);
    EXPECT_EQ(1u, function.has_variadic_parameters);
    EXPECT_EQ(1u, function.parameter_count);
    diagnostic = FindFunctionDiagnostic(
        analyzed.get(), 0u, QL_C_FRONTEND_DIAGNOSTIC_VARIADIC_FUNCTION);
    EXPECT_EQ(QL_C_FRONTEND_DIAGNOSTIC_VARIADIC_FUNCTION, diagnostic.code);
    EXPECT_STREQ("variadic_parameter", diagnostic.construct_kind);
}

TEST(CFrontend, ClassifiesKAndRDefinitionAsUnsupported) {
    constexpr char source[] =
        "int sum(a, b)\n"
        "int a;\n"
        "int b;\n"
        "{ return a + b; }\n";
    CFrontendUnit analyzed;
    ql_c_function_view function{};
    ql_c_frontend_diagnostic_view diagnostic{};
    ql_error error{};

    ASSERT_EQ(QL_STATUS_OK,
              ql_c_frontend_analyze(nullptr, source, sizeof(source) - 1u,
                                    analyzed.output(), &error))
        << error.message;
    function.struct_size = sizeof(function);
    ASSERT_EQ(QL_STATUS_OK,
              ql_c_frontend_function_at(analyzed.get(), 0u, &function,
                                        &error));
    EXPECT_EQ(QL_C_FUNCTION_UNSUPPORTED, function.support);
    EXPECT_EQ(1u, function.has_old_style_parameters);
    EXPECT_EQ(0u, function.parameter_count);
    diagnostic = FindFunctionDiagnostic(
        analyzed.get(), 0u, QL_C_FRONTEND_DIAGNOSTIC_OLD_STYLE_FUNCTION);
    EXPECT_EQ(QL_C_FRONTEND_DIAGNOSTIC_OLD_STYLE_FUNCTION, diagnostic.code);
}

TEST(CFrontend, RequiresPreprocessedInputInsteadOfExpandingMacros) {
    constexpr char source[] =
        "#define ONE 1\n"
        "int value(void) { return ONE; }\n";
    CFrontendUnit analyzed;
    ql_c_frontend_unit_view unit{};
    ql_c_function_view function{};
    ql_c_frontend_diagnostic_view diagnostic{};
    ql_error error{};

    ASSERT_EQ(QL_STATUS_OK,
              ql_c_frontend_analyze(nullptr, source, sizeof(source) - 1u,
                                    analyzed.output(), &error))
        << error.message;
    unit.struct_size = sizeof(unit);
    ASSERT_EQ(QL_STATUS_OK,
              ql_c_frontend_unit_get_view(analyzed.get(), &unit, &error));
    EXPECT_EQ(QL_C_FUNCTION_UNSUPPORTED, unit.support);
    function.struct_size = sizeof(function);
    ASSERT_EQ(QL_STATUS_OK,
              ql_c_frontend_select_function(analyzed.get(), "value", 5u,
                                            &function, &error));
    EXPECT_EQ(QL_C_FUNCTION_UNSUPPORTED, function.support);
    diagnostic = FindFunctionDiagnostic(
        analyzed.get(), function.index,
        QL_C_FRONTEND_DIAGNOSTIC_PREPROCESSING_REQUIRED);
    EXPECT_EQ(QL_C_FRONTEND_DIAGNOSTIC_PREPROCESSING_REQUIRED,
              diagnostic.code);
    EXPECT_STREQ("preproc_def", diagnostic.construct_kind);
}

TEST(CFrontend, ReportsUnsupportedExtensionWithSourceRange) {
    constexpr char source[] =
        "int barrier(int value) {\n"
        "  __asm__ volatile(\"\" : \"+r\"(value));\n"
        "  return value;\n"
        "}\n";
    CFrontendUnit analyzed;
    ql_c_function_view function{};
    ql_c_frontend_diagnostic_view diagnostic{};
    ql_error error{};

    ASSERT_EQ(QL_STATUS_OK,
              ql_c_frontend_analyze(nullptr, source, sizeof(source) - 1u,
                                    analyzed.output(), &error))
        << error.message;
    function.struct_size = sizeof(function);
    ASSERT_EQ(QL_STATUS_OK,
              ql_c_frontend_function_at(analyzed.get(), 0u, &function,
                                        &error));
    EXPECT_EQ(QL_C_FUNCTION_UNSUPPORTED, function.support);
    diagnostic = FindFunctionDiagnostic(
        analyzed.get(), 0u,
        QL_C_FRONTEND_DIAGNOSTIC_UNSUPPORTED_CONSTRUCT);
    EXPECT_EQ(QL_C_FRONTEND_DIAGNOSTIC_UNSUPPORTED_CONSTRUCT,
              diagnostic.code);
    EXPECT_STREQ("gnu_asm_expression", diagnostic.construct_kind);
    EXPECT_LT(diagnostic.range.start_byte, diagnostic.range.end_byte);
}

TEST(CFrontend, RejectsRecoveredSyntaxTreeAsParseError) {
    constexpr char source[] = "int broken( { return 0; }";
    CFrontendUnit analyzed;
    ql_error error{};

    EXPECT_EQ(QL_STATUS_PARSE_ERROR,
              ql_c_frontend_analyze(nullptr, source, sizeof(source) - 1u,
                                    analyzed.output(), &error));
    EXPECT_EQ(nullptr, analyzed.get());
    EXPECT_NE(nullptr, std::strstr(error.message, "syntax error"));
}

TEST(CFrontend, RejectsAmbiguousFunctionSelection) {
    constexpr char source[] =
        "int repeated(void) { return 1; }\n"
        "int repeated(void) { return 2; }\n";
    CFrontendUnit analyzed;
    ql_c_frontend_unit_view unit{};
    ql_c_function_view function{};
    ql_error error{};

    ASSERT_EQ(QL_STATUS_OK,
              ql_c_frontend_analyze(nullptr, source, sizeof(source) - 1u,
                                    analyzed.output(), &error))
        << error.message;
    unit.struct_size = sizeof(unit);
    ASSERT_EQ(QL_STATUS_OK,
              ql_c_frontend_unit_get_view(analyzed.get(), &unit, &error));
    EXPECT_EQ(QL_C_FUNCTION_UNSUPPORTED, unit.support);
    function.struct_size = sizeof(function);
    EXPECT_EQ(QL_STATUS_ALREADY_EXISTS,
              ql_c_frontend_select_function(analyzed.get(), "repeated", 8u,
                                            &function, &error));
}

TEST(CFrontend, EnforcesVersionedViewSizes) {
    constexpr char source[] = "int value(void) { return 1; }";
    CFrontendUnit analyzed;
    ql_c_frontend_unit_view too_small{};
    ql_error error{};

    ASSERT_EQ(QL_STATUS_OK,
              ql_c_frontend_analyze(nullptr, source, sizeof(source) - 1u,
                                    analyzed.output(), &error))
        << error.message;
    too_small.struct_size = 1u;
    EXPECT_EQ(QL_STATUS_ABI_MISMATCH,
              ql_c_frontend_unit_get_view(analyzed.get(), &too_small,
                                          &error));
}

}  // namespace
