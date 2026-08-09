#include "quodlibet/quodlibet.h"

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>

#include <gtest/gtest.h>

namespace {

bool find_recovery_node(ql_c_syntax_tree *tree,
                        ql_c_syntax_node_view *recovery) {
    ql_c_syntax_cursor *cursor = nullptr;
    ql_error error{};

    if (ql_c_syntax_cursor_create(tree, &cursor, &error) != QL_STATUS_OK) {
        return false;
    }
    for (;;) {
        ql_c_syntax_node_view view{};
        view.struct_size = sizeof(view);
        if (ql_c_syntax_cursor_current(cursor, &view, &error) !=
            QL_STATUS_OK) {
            ql_c_syntax_cursor_destroy(cursor);
            return false;
        }
        if ((view.flags & (QL_C_SYNTAX_NODE_ERROR |
                           QL_C_SYNTAX_NODE_MISSING)) != 0u) {
            *recovery = view;
            ql_c_syntax_cursor_destroy(cursor);
            return true;
        }
        if (ql_c_syntax_cursor_goto_first_child(cursor) != 0u) {
            continue;
        }
        while (ql_c_syntax_cursor_goto_next_sibling(cursor) == 0u) {
            if (ql_c_syntax_cursor_goto_parent(cursor) == 0u) {
                ql_c_syntax_cursor_destroy(cursor);
                return false;
            }
        }
    }
}

TEST(CSyntax, ParsesTranslationUnitAndSupportsCursorTraversal) {
    constexpr char source[] =
        "#define INC(x) ((x) + 1)\n"
        "int increment(int value) { return INC(value); }\n";
    ql_c_parser *parser = nullptr;
    ql_c_syntax_tree *tree = nullptr;
    ql_c_syntax_cursor *cursor = nullptr;
    ql_c_syntax_node_view root{};
    ql_c_syntax_node_view child{};
    ql_error error{};

    root.struct_size = sizeof(root);
    child.struct_size = sizeof(child);
    ASSERT_EQ(QL_STATUS_OK, ql_c_parser_create(nullptr, &parser, &error))
        << error.message;
    ASSERT_EQ(QL_STATUS_OK,
              ql_c_parser_parse(parser, source, sizeof(source) - 1u, &tree,
                                &error))
        << error.message;
    EXPECT_EQ(0u, ql_c_syntax_tree_has_errors(tree));
    ASSERT_EQ(QL_STATUS_OK, ql_c_syntax_tree_root(tree, &root, &error));
    EXPECT_STREQ("translation_unit", root.kind);
    EXPECT_EQ(sizeof(source) - 1u, root.range.end_byte);
    EXPECT_NE(0u, root.flags & QL_C_SYNTAX_NODE_NAMED);
    EXPECT_EQ(0u, root.flags & QL_C_SYNTAX_NODE_HAS_ERROR);
    EXPECT_GE(root.named_child_count, 2u);

    ASSERT_EQ(QL_STATUS_OK,
              ql_c_syntax_cursor_create(tree, &cursor, &error));
    ASSERT_EQ(1u, ql_c_syntax_cursor_goto_first_child(cursor));
    ASSERT_EQ(QL_STATUS_OK,
              ql_c_syntax_cursor_current(cursor, &child, &error));
    EXPECT_EQ(0u, child.range.start_byte);
    ql_c_syntax_cursor_reset(cursor);
    ASSERT_EQ(QL_STATUS_OK,
              ql_c_syntax_cursor_current(cursor, &child, &error));
    EXPECT_STREQ("translation_unit", child.kind);

    ql_c_syntax_cursor_destroy(cursor);
    ql_c_syntax_tree_destroy(tree);
    ql_c_parser_destroy(parser);
}

TEST(CSyntax, ReturnsRecoveredTreeAndIdentifiesSyntaxError) {
    constexpr char malformed[] = "int broken( { return 0; }\n";
    ql_c_parser *parser = nullptr;
    ql_c_syntax_tree *tree = nullptr;
    ql_c_syntax_node_view recovery{};
    ql_error error{};

    recovery.struct_size = sizeof(recovery);
    ASSERT_EQ(QL_STATUS_OK, ql_c_parser_create(nullptr, &parser, &error));
    ASSERT_EQ(QL_STATUS_OK,
              ql_c_parser_parse(parser, malformed, sizeof(malformed) - 1u,
                                &tree, &error));
    EXPECT_EQ(1u, ql_c_syntax_tree_has_errors(tree));
    EXPECT_TRUE(find_recovery_node(tree, &recovery));
    EXPECT_TRUE((recovery.flags & QL_C_SYNTAX_NODE_ERROR) != 0u ||
                (recovery.flags & QL_C_SYNTAX_NODE_MISSING) != 0u);

    ql_c_syntax_tree_destroy(tree);
    ql_c_parser_destroy(parser);
}

TEST(CSyntax, ReusesParserAfterMalformedInput) {
    constexpr char malformed[] = "int f( {";
    constexpr char valid[] = "int f(void) { return 0; }";
    ql_c_parser *parser = nullptr;
    ql_c_syntax_tree *first = nullptr;
    ql_c_syntax_tree *second = nullptr;
    ql_error error{};

    ASSERT_EQ(QL_STATUS_OK, ql_c_parser_create(nullptr, &parser, &error));
    ASSERT_EQ(QL_STATUS_OK,
              ql_c_parser_parse(parser, malformed, sizeof(malformed) - 1u,
                                &first, &error));
    EXPECT_EQ(1u, ql_c_syntax_tree_has_errors(first));
    ASSERT_EQ(QL_STATUS_OK,
              ql_c_parser_parse(parser, valid, sizeof(valid) - 1u, &second,
                                &error));
    EXPECT_EQ(0u, ql_c_syntax_tree_has_errors(second));

    ql_c_syntax_tree_destroy(second);
    ql_c_syntax_tree_destroy(first);
    ql_c_parser_destroy(parser);
}

TEST(CSyntax, CursorKeepsTreeAlive) {
    constexpr char source[] = "int value;";
    ql_c_parser *parser = nullptr;
    ql_c_syntax_tree *tree = nullptr;
    ql_c_syntax_cursor *cursor = nullptr;
    ql_c_syntax_node_view view{};
    ql_error error{};

    view.struct_size = sizeof(view);
    ASSERT_EQ(QL_STATUS_OK, ql_c_parser_create(nullptr, &parser, &error));
    ASSERT_EQ(QL_STATUS_OK,
              ql_c_parser_parse(parser, source, sizeof(source) - 1u, &tree,
                                &error));
    ASSERT_EQ(QL_STATUS_OK,
              ql_c_syntax_cursor_create(tree, &cursor, &error));
    ql_c_syntax_tree_destroy(tree);
    ASSERT_EQ(QL_STATUS_OK,
              ql_c_syntax_cursor_current(cursor, &view, &error));
    EXPECT_STREQ("translation_unit", view.kind);

    ql_c_syntax_cursor_destroy(cursor);
    ql_c_parser_destroy(parser);
}

TEST(CSyntax, RejectsInputBeyondTreeSitterLimit) {
    if constexpr (std::numeric_limits<std::size_t>::max() > UINT32_MAX) {
        ql_c_parser *parser = nullptr;
        ql_c_syntax_tree *tree = nullptr;
        ql_error error{};

        ASSERT_EQ(QL_STATUS_OK,
                  ql_c_parser_create(nullptr, &parser, &error));
        EXPECT_EQ(QL_STATUS_INVALID_ARGUMENT,
                  ql_c_parser_parse(
                      parser, "x", static_cast<std::size_t>(UINT32_MAX) + 1u,
                      &tree, &error));
        EXPECT_EQ(nullptr, tree);
        ql_c_parser_destroy(parser);
    }
}

}  // namespace
