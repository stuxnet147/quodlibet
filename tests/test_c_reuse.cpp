/* The reuse entry points, which exist so a caller working through a corpus
   does not pay for a parser per unit or parse the same source twice.

   What has to hold is that they are the same functions: reusing a parser or
   handing back an already-parsed tree must produce the identical unit and the
   identical IR, byte for byte, or the fast path would be a second
   implementation with its own bugs. */

#include "quodlibet/artifact.h"
#include "quodlibet/c_lower.h"

#include <cstddef>
#include <cstring>
#include <string>
#include <vector>

#include <gtest/gtest.h>

namespace {

const char *const kSources[] = {
    "int one(int a) { return a + 1; }",
    "int two(int a, int b) { return a / b; }",
    "typedef int T;\nT three(T a) { return a * 3; }",
    "struct S { int f; int g; };\nint four(struct S *p) { return p->g; }",
    "void five(int a) { int v = a; int *p = &v; *p = *p + 1; }",
};

class Unit {
public:
    ~Unit() { ql_c_frontend_unit_destroy(unit_); }
    ql_c_frontend_unit **output() { return &unit_; }
    ql_c_frontend_unit *get() const { return unit_; }

private:
    ql_c_frontend_unit *unit_ = nullptr;
};

ql_c_frontend_unit_view UnitView(const ql_c_frontend_unit *unit) {
    ql_c_frontend_unit_view view{};
    ql_error error{};
    view.struct_size = sizeof(view);
    EXPECT_EQ(QL_STATUS_OK,
              ql_c_frontend_unit_get_view(unit, &view, &error))
        << error.message;
    return view;
}

/* The digest of the IR a lowering produced, or an empty string when it
   produced none. Comparing digests is what makes "identical" mean identical
   rather than merely similar. */
std::string LoweredDigest(const ql_c_lower_result *result) {
    ql_c_lower_result_view_v1 view{};
    ql_artifact_view artifact{};
    ql_error error{};
    view.struct_size = sizeof(view);
    EXPECT_EQ(QL_STATUS_OK,
              ql_c_lower_result_get_view(result, &view, &error));
    if (view.support != QL_C_LOWER_SUPPORTED || view.ir_artifact == nullptr) {
        return std::string();
    }
    artifact.struct_size = sizeof(artifact);
    EXPECT_EQ(QL_STATUS_OK,
              ql_artifact_get_view(view.ir_artifact, &artifact, &error));
    return std::string(reinterpret_cast<const char *>(artifact.digest.bytes),
                       sizeof(artifact.digest.bytes));
}

const char *FunctionName(const char *source) {
    static std::string name;
    const std::string text(source);
    const std::size_t open = text.find('(');
    std::size_t start = text.rfind(' ', open);
    name = text.substr(start + 1u, open - start - 1u);
    return name.c_str();
}

TEST(CReuse, AParserCanServeManyUnitsAndSurvivesThem) {
    ql_c_parser *parser = nullptr;
    ql_error error{};
    ASSERT_EQ(QL_STATUS_OK, ql_c_parser_create(nullptr, &parser, &error))
        << error.message;

    for (const char *source : kSources) {
        Unit shared;
        Unit fresh;
        SCOPED_TRACE(source);
        ASSERT_EQ(QL_STATUS_OK,
                  ql_c_frontend_analyze_with_parser(
                      nullptr, parser, source, std::strlen(source),
                      shared.output(), &error))
            << error.message;
        ASSERT_EQ(QL_STATUS_OK,
                  ql_c_frontend_analyze(nullptr, source, std::strlen(source),
                                        fresh.output(), &error))
            << error.message;
        const ql_c_frontend_unit_view a = UnitView(shared.get());
        const ql_c_frontend_unit_view b = UnitView(fresh.get());
        EXPECT_EQ(b.support, a.support);
        EXPECT_EQ(b.function_count, a.function_count);
        EXPECT_EQ(b.diagnostic_count, a.diagnostic_count);
    }

    /* The parser was borrowed, not consumed, so it still parses. */
    ql_c_syntax_tree *tree = nullptr;
    const char *last = kSources[0];
    ASSERT_EQ(QL_STATUS_OK,
              ql_c_parser_parse(parser, last, std::strlen(last), &tree,
                                &error))
        << error.message;
    EXPECT_EQ(0u, ql_c_syntax_tree_has_errors(tree));
    ql_c_syntax_tree_destroy(tree);
    ql_c_parser_destroy(parser);
}

TEST(CReuse, ANullParserIsTheOrdinaryEntryPoint) {
    Unit shared;
    Unit fresh;
    ql_error error{};
    const char *source = kSources[3];
    ASSERT_EQ(QL_STATUS_OK,
              ql_c_frontend_analyze_with_parser(nullptr, nullptr, source,
                                                std::strlen(source),
                                                shared.output(), &error))
        << error.message;
    ASSERT_EQ(QL_STATUS_OK,
              ql_c_frontend_analyze(nullptr, source, std::strlen(source),
                                    fresh.output(), &error))
        << error.message;
    EXPECT_EQ(UnitView(fresh.get()).function_count,
              UnitView(shared.get()).function_count);
}

TEST(CReuse, LoweringFromABorrowedTreeGivesTheIdenticalIr) {
    ql_c_parser *parser = nullptr;
    ql_error error{};
    ASSERT_EQ(QL_STATUS_OK, ql_c_parser_create(nullptr, &parser, &error));

    for (const char *source : kSources) {
        const std::size_t size = std::strlen(source);
        const char *name = FunctionName(source);
        Unit unit;
        ql_c_syntax_tree *tree = nullptr;
        ql_c_function_view function{};
        ql_c_lower_result *reused = nullptr;
        ql_c_lower_result *plain = nullptr;
        SCOPED_TRACE(source);

        ASSERT_EQ(QL_STATUS_OK,
                  ql_c_frontend_analyze_with_parser(nullptr, parser, source,
                                                    size, unit.output(),
                                                    &error))
            << error.message;
        ASSERT_EQ(QL_STATUS_OK,
                  ql_c_parser_parse(parser, source, size, &tree, &error))
            << error.message;
        function.struct_size = sizeof(function);
        ASSERT_EQ(QL_STATUS_OK,
                  ql_c_frontend_select_function(unit.get(), name,
                                                std::strlen(name), &function,
                                                &error))
            << error.message;

        ASSERT_EQ(QL_STATUS_OK,
                  ql_c_lower_selected_function_with_tree(
                      nullptr, source, size, unit.get(), &function, tree,
                      &reused, &error))
            << error.message;
        ASSERT_EQ(QL_STATUS_OK,
                  ql_c_lower_selected_function(nullptr, source, size,
                                               unit.get(), &function, &plain,
                                               &error))
            << error.message;
        EXPECT_EQ(LoweredDigest(plain), LoweredDigest(reused));
        ql_c_lower_result_destroy(reused);
        ql_c_lower_result_destroy(plain);

        /* The tree was borrowed, so it is still ours to use and to free. */
        EXPECT_EQ(0u, ql_c_syntax_tree_has_errors(tree));
        ql_c_syntax_tree_destroy(tree);
    }
    ql_c_parser_destroy(parser);
}

TEST(CReuse, RefusesATreeThatIsNotThisSource) {
    /* Nothing checks the tree against the bytes directly, but the lowering
       already insists that the selected function's range and body range are
       exactly what it finds. A tree of other text fails that, so the wrong
       program is refused rather than lowered. */
    ql_c_parser *parser = nullptr;
    Unit unit;
    ql_c_syntax_tree *other = nullptr;
    ql_c_function_view function{};
    ql_c_lower_result *result = nullptr;
    ql_error error{};
    const char *source = "int only(int a) { return a + 1; }";
    const char *elsewhere =
        "int filler(void) { return 0; }\nint only(int a) { return a + 1; }";

    ASSERT_EQ(QL_STATUS_OK, ql_c_parser_create(nullptr, &parser, &error));
    ASSERT_EQ(QL_STATUS_OK,
              ql_c_frontend_analyze(nullptr, source, std::strlen(source),
                                    unit.output(), &error));
    ASSERT_EQ(QL_STATUS_OK,
              ql_c_parser_parse(parser, elsewhere, std::strlen(elsewhere),
                                &other, &error));
    function.struct_size = sizeof(function);
    ASSERT_EQ(QL_STATUS_OK,
              ql_c_frontend_select_function(unit.get(), "only", 4u, &function,
                                            &error));
    EXPECT_NE(QL_STATUS_OK,
              ql_c_lower_selected_function_with_tree(
                  nullptr, source, std::strlen(source), unit.get(), &function,
                  other, &result, &error));
    ql_c_lower_result_destroy(result);
    ql_c_syntax_tree_destroy(other);
    ql_c_parser_destroy(parser);
}

}  // namespace
