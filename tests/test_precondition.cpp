#include "quodlibet/precondition.h"

#include <array>
#include <cstddef>
#include <cstring>
#include <string>

#include <gtest/gtest.h>

namespace {

struct TestSignature {
    std::array<ql_signature_argument_v1, 7> arguments{};
    ql_signature_view_v1 view{};

    TestSignature() {
        for (auto &argument : arguments) {
            ql_signature_argument_init(&argument);
        }
        arguments[0].kind = QL_SIGNATURE_ARGUMENT_SIGNED_INTEGER;
        arguments[0].bit_width = 32u;
        arguments[1].kind = QL_SIGNATURE_ARGUMENT_UNSIGNED_INTEGER;
        arguments[1].bit_width = 32u;
        arguments[2].kind = QL_SIGNATURE_ARGUMENT_POINTER;
        arguments[2].bit_width = 64u;
        arguments[3].kind = QL_SIGNATURE_ARGUMENT_UNSIGNED_INTEGER;
        arguments[3].bit_width = 64u;
        arguments[4].kind = QL_SIGNATURE_ARGUMENT_POINTER;
        arguments[4].bit_width = 64u;
        arguments[5].kind = QL_SIGNATURE_ARGUMENT_BOOL;
        arguments[5].bit_width = 1u;
        arguments[6].kind = QL_SIGNATURE_ARGUMENT_SIGNED_INTEGER;
        arguments[6].bit_width = 64u;

        ql_signature_view_init(&view);
        view.arguments = arguments.data();
        view.argument_count = arguments.size();
    }
};

ql_status parse(const ql_signature_view_v1 &signature, const char *json,
                ql_precondition **output, ql_error *error) {
    return ql_precondition_parse(nullptr, json,
                                 json != nullptr ? std::strlen(json) : 0u,
                                 &signature, output, error);
}

TEST(Precondition, ParsesTypedPointerAndIntegerContractCanonically) {
    TestSignature signature;
    const char *json = R"json(
      {
        "expression": {
          "args": [
            {"right":{"op":"int","value":"00010","width":32,"signed":true},
             "left":{"op":"sadd","right":{"op":"int","signed":true,"width":32,"value":"+001"},
                     "left":{"index":0,"op":"arg"}},"op":"slt"},
            {"op":"ult","left":{"op":"uadd","left":{"op":"arg","index":1},
                     "right":{"op":"int","signed":false,"width":32,"value":"1"}},
                     "right":{"op":"int","signed":false,"width":32,"value":"100"}},
            {"op":"valid_range","range":{
                     "pointer":{"op":"arg","index":2},
                     "offset":{"op":"arg","index":6},
                     "bytes":{"op":"arg","index":3}},
                     "read":true,"write":true,"alignment":16,
                     "nullable":false,"alias_group":7},
            {"op":"aligned","pointer":{"op":"arg","index":2},
                     "offset":{"op":"int","signed":true,"width":64,"value":"0"},
                     "alignment":16},
            {"op":"disjoint",
                     "left":{"pointer":{"op":"arg","index":2},
                             "offset":{"op":"int","signed":true,"width":64,"value":"0"},
                             "bytes":{"op":"arg","index":3}},
                     "right":{"pointer":{"op":"arg","index":4},
                              "offset":{"op":"int","signed":true,"width":64,"value":"0"},
                              "bytes":{"op":"arg","index":3}}},
            {"op":"implies","left":{"op":"arg","index":5},
                     "right":{"op":"ne","left":{"op":"arg","index":2},
                              "right":{"op":"arg","index":4}}}
          ],
          "op": "and"
        },
        "schema_version": 1
      })json";
    ql_precondition *precondition = nullptr;
    ql_precondition *round_trip = nullptr;
    ql_precondition_view_v1 view{};
    ql_precondition_view_v1 round_trip_view{};
    ql_precondition_node_view_v1 root{};
    ql_error error{};

    ASSERT_EQ(QL_STATUS_OK,
              parse(signature.view, json, &precondition, &error))
        << error.message;
    view.struct_size = sizeof(view);
    ASSERT_EQ(QL_STATUS_OK,
              ql_precondition_get_view(precondition, &view, &error));
    ASSERT_NE(nullptr, view.canonical_bytes);
    EXPECT_EQ(view.canonical_size, std::strlen(view.canonical_bytes));
    EXPECT_NE(std::string::npos,
              std::string(view.canonical_bytes).find("\"value\":\"1\""));
    EXPECT_EQ(std::string::npos,
              std::string(view.canonical_bytes).find("+001"));

    root.struct_size = sizeof(root);
    ASSERT_EQ(QL_STATUS_OK,
              ql_precondition_node_at(precondition, view.root_node, &root,
                                      &error));
    EXPECT_EQ(QL_PRECONDITION_NODE_AND, root.kind);
    EXPECT_EQ(6u, root.child_count);

    ql_precondition_node_view_v1 valid_range{};
    valid_range.struct_size = sizeof(valid_range);
    ASSERT_EQ(QL_STATUS_OK,
              ql_precondition_node_at(precondition, root.children[2],
                                      &valid_range, &error));
    EXPECT_EQ(QL_PRECONDITION_NODE_VALID_RANGE, valid_range.kind);
    EXPECT_EQ(static_cast<uint32_t>(QL_PRECONDITION_ACCESS_READ |
                                    QL_PRECONDITION_ACCESS_WRITE),
              valid_range.access);
    EXPECT_EQ(16u, valid_range.alignment);
    EXPECT_EQ(0u, valid_range.nullable);
    EXPECT_EQ(7u, valid_range.alias_group);

    ASSERT_EQ(QL_STATUS_OK,
              ql_precondition_parse(nullptr, view.canonical_bytes,
                                    view.canonical_size, &signature.view,
                                    &round_trip, &error))
        << error.message;
    round_trip_view.struct_size = sizeof(round_trip_view);
    ASSERT_EQ(QL_STATUS_OK, ql_precondition_get_view(
                                round_trip, &round_trip_view, &error));
    EXPECT_TRUE(ql_digest_equal(&view.digest, &round_trip_view.digest));
    EXPECT_EQ(view.canonical_size, round_trip_view.canonical_size);
    EXPECT_EQ(0, std::memcmp(view.canonical_bytes,
                             round_trip_view.canonical_bytes,
                             view.canonical_size));

    ql_precondition_destroy(round_trip);
    ql_precondition_destroy(precondition);
}

TEST(Precondition, DefaultTrueContractIsImmutableAndSignatureBound) {
    TestSignature first_signature;
    TestSignature second_signature;
    ql_precondition *first = nullptr;
    ql_precondition *second = nullptr;
    ql_precondition_view_v1 first_view{};
    ql_precondition_view_v1 second_view{};
    ql_error error{};

    second_signature.arguments[0].kind =
        QL_SIGNATURE_ARGUMENT_UNSIGNED_INTEGER;
    ASSERT_EQ(QL_STATUS_OK,
              parse(first_signature.view, nullptr, &first, &error));
    ASSERT_EQ(QL_STATUS_OK,
              parse(second_signature.view, nullptr, &second, &error));
    first_view.struct_size = sizeof(first_view);
    second_view.struct_size = sizeof(second_view);
    ASSERT_EQ(QL_STATUS_OK,
              ql_precondition_get_view(first, &first_view, &error));
    ASSERT_EQ(QL_STATUS_OK,
              ql_precondition_get_view(second, &second_view, &error));
    EXPECT_STREQ("{\"schema_version\":1,\"expression\":true}",
                 first_view.canonical_bytes);
    EXPECT_FALSE(ql_digest_equal(&first_view.signature_digest,
                                 &second_view.signature_digest));
    EXPECT_FALSE(ql_digest_equal(&first_view.digest, &second_view.digest));
    ql_precondition_destroy(second);
    ql_precondition_destroy(first);
}

TEST(Precondition, SignedAndUnsignedOperatorsRejectImplicitCoercion) {
    TestSignature signature;
    const char *mixed = R"json(
      {"schema_version":1,"expression":{"op":"slt",
       "left":{"op":"arg","index":0},
       "right":{"op":"arg","index":1}}})json";
    const char *wrong_unsigned = R"json(
      {"schema_version":1,"expression":{"op":"uadd",
       "left":{"op":"arg","index":0},
       "right":{"op":"int","signed":true,"width":32,"value":"1"}}})json";
    ql_precondition *precondition = nullptr;
    ql_error error{};

    EXPECT_EQ(QL_STATUS_TYPE_MISMATCH,
              parse(signature.view, mixed, &precondition, &error));
    EXPECT_EQ(nullptr, precondition);
    EXPECT_EQ(QL_STATUS_TYPE_MISMATCH,
              parse(signature.view, wrong_unsigned, &precondition, &error));
    EXPECT_EQ(nullptr, precondition);
}

TEST(Precondition, EveryV1ComparisonAndArithmeticOperatorIsTyped) {
    TestSignature signature;
    const std::array<const char *, 4> signed_comparisons = {"slt", "sle",
                                                            "sgt", "sge"};
    const std::array<const char *, 4> unsigned_comparisons = {"ult", "ule",
                                                              "ugt", "uge"};
    const std::array<const char *, 3> signed_arithmetic = {"sadd", "ssub",
                                                           "smul"};
    const std::array<const char *, 3> unsigned_arithmetic = {"uadd", "usub",
                                                             "umul"};
    ql_error error{};

    auto expect_comparison = [&](const char *op, bool is_signed,
                                 unsigned argument) {
        std::string json =
            "{\"schema_version\":1,\"expression\":{\"op\":\"" +
            std::string(op) +
            "\",\"left\":{\"op\":\"arg\",\"index\":" +
            std::to_string(argument) +
            "},\"right\":{\"op\":\"int\",\"signed\":" +
            (is_signed ? "true" : "false") +
            ",\"width\":32,\"value\":\"7\"}}}";
        ql_precondition *precondition = nullptr;
        ASSERT_EQ(QL_STATUS_OK,
                  ql_precondition_parse(nullptr, json.data(), json.size(),
                                        &signature.view, &precondition,
                                        &error))
            << op << ": " << error.message;
        ql_precondition_destroy(precondition);
    };
    auto expect_arithmetic = [&](const char *op, bool is_signed,
                                 unsigned argument) {
        std::string operand =
            "{\"op\":\"" + std::string(op) +
            "\",\"left\":{\"op\":\"arg\",\"index\":" +
            std::to_string(argument) +
            "},\"right\":{\"op\":\"int\",\"signed\":" +
            (is_signed ? "true" : "false") +
            ",\"width\":32,\"value\":\"1\"}}";
        std::string json =
            "{\"schema_version\":1,\"expression\":{\"op\":\"eq\","
            "\"left\":" +
            operand + ",\"right\":{\"op\":\"arg\",\"index\":" +
            std::to_string(argument) + "}}}";
        ql_precondition *precondition = nullptr;
        ASSERT_EQ(QL_STATUS_OK,
                  ql_precondition_parse(nullptr, json.data(), json.size(),
                                        &signature.view, &precondition,
                                        &error))
            << op << ": " << error.message;
        ql_precondition_destroy(precondition);
    };

    for (const char *op : signed_comparisons) {
        expect_comparison(op, true, 0u);
    }
    for (const char *op : unsigned_comparisons) {
        expect_comparison(op, false, 1u);
    }
    for (const char *op : signed_arithmetic) {
        expect_arithmetic(op, true, 0u);
    }
    for (const char *op : unsigned_arithmetic) {
        expect_arithmetic(op, false, 1u);
    }
}

TEST(Precondition, IntegerConstantsMustFitDeclaredWidthExactly) {
    TestSignature signature;
    const std::array<const char *, 4> invalid = {
        R"json({"schema_version":1,"expression":{"op":"eq","left":{"op":"int","signed":true,"width":8,"value":"128"},"right":{"op":"int","signed":true,"width":8,"value":"0"}}})json",
        R"json({"schema_version":1,"expression":{"op":"eq","left":{"op":"int","signed":true,"width":8,"value":"-129"},"right":{"op":"int","signed":true,"width":8,"value":"0"}}})json",
        R"json({"schema_version":1,"expression":{"op":"eq","left":{"op":"int","signed":false,"width":8,"value":"256"},"right":{"op":"int","signed":false,"width":8,"value":"0"}}})json",
        R"json({"schema_version":1,"expression":{"op":"eq","left":{"op":"int","signed":false,"width":8,"value":"-1"},"right":{"op":"int","signed":false,"width":8,"value":"0"}}})json"};
    ql_error error{};

    for (const char *json : invalid) {
        ql_precondition *precondition = nullptr;
        EXPECT_EQ(QL_STATUS_TYPE_MISMATCH,
                  parse(signature.view, json, &precondition, &error));
        EXPECT_EQ(nullptr, precondition);
    }
}

TEST(Precondition, PointerRangesRejectZeroLengthWrongWidthAndBadProperties) {
    TestSignature signature;
    const char *zero_length = R"json(
      {"schema_version":1,"expression":{"op":"valid_range",
       "range":{"pointer":{"op":"arg","index":2},
                "offset":{"op":"int","signed":true,"width":64,"value":"0"},
                "bytes":{"op":"int","signed":false,"width":64,"value":"0"}},
       "read":true,"write":false,"alignment":8,"nullable":false,"alias_group":0}})json";
    const char *wrong_width = R"json(
      {"schema_version":1,"expression":{"op":"valid_range",
       "range":{"pointer":{"op":"arg","index":2},
                "offset":{"op":"int","signed":true,"width":32,"value":"0"},
                "bytes":{"op":"arg","index":3}},
       "read":true,"write":false,"alignment":8,"nullable":false,"alias_group":0}})json";
    const char *bad_properties = R"json(
      {"schema_version":1,"expression":{"op":"valid_range",
       "range":{"pointer":{"op":"arg","index":2},
                "offset":{"op":"arg","index":6},
                "bytes":{"op":"arg","index":3}},
       "read":false,"write":false,"alignment":3,"nullable":false,"alias_group":0}})json";
    ql_precondition *precondition = nullptr;
    ql_error error{};

    EXPECT_EQ(QL_STATUS_INVALID_ARGUMENT,
              parse(signature.view, zero_length, &precondition, &error));
    EXPECT_EQ(QL_STATUS_TYPE_MISMATCH,
              parse(signature.view, wrong_width, &precondition, &error));
    EXPECT_EQ(QL_STATUS_INVALID_ARGUMENT,
              parse(signature.view, bad_properties, &precondition, &error));
    EXPECT_EQ(nullptr, precondition);
}

TEST(Precondition, StaticallyEmptyDomainsAreRejected) {
    TestSignature signature;
    const char *contradiction = R"json(
      {"schema_version":1,"expression":{"op":"and","args":[
       {"op":"slt","left":{"op":"arg","index":0},
        "right":{"op":"int","signed":true,"width":32,"value":"10"}},
       {"op":"sge","left":{"op":"arg","index":0},
        "right":{"op":"int","signed":true,"width":32,"value":"10"}}]}})json";
    const char *same_range = R"json(
      {"schema_version":1,"expression":{"op":"disjoint",
       "left":{"pointer":{"op":"arg","index":2},
               "offset":{"op":"arg","index":6},
               "bytes":{"op":"arg","index":3}},
       "right":{"pointer":{"op":"arg","index":2},
                "offset":{"op":"arg","index":6},
                "bytes":{"op":"arg","index":3}}}})json";
    const char *conflicting_alias_groups = R"json(
      {"schema_version":1,"expression":{"op":"and","args":[
       {"op":"valid_range","range":{
          "pointer":{"op":"arg","index":2},
          "offset":{"op":"arg","index":6},
          "bytes":{"op":"arg","index":3}},
        "read":true,"write":false,"alignment":8,"nullable":false,"alias_group":1},
       {"op":"valid_range","range":{
          "pointer":{"op":"arg","index":2},
          "offset":{"op":"arg","index":6},
          "bytes":{"op":"arg","index":3}},
        "read":true,"write":false,"alignment":8,"nullable":false,"alias_group":2}
      ]}})json";
    ql_precondition *precondition = nullptr;
    ql_error error{};

    EXPECT_EQ(QL_STATUS_INVALID_ARGUMENT,
              parse(signature.view,
                    "{\"schema_version\":1,\"expression\":false}",
                    &precondition, &error));
    EXPECT_NE(nullptr, std::strstr(error.message, "empty input domain"));
    EXPECT_EQ(QL_STATUS_INVALID_ARGUMENT,
              parse(signature.view, contradiction, &precondition, &error));
    EXPECT_EQ(QL_STATUS_INVALID_ARGUMENT,
              parse(signature.view, same_range, &precondition, &error));
    EXPECT_EQ(QL_STATUS_INVALID_ARGUMENT,
              parse(signature.view, conflicting_alias_groups, &precondition,
                    &error));
    EXPECT_EQ(nullptr, precondition);
}

TEST(Precondition, SchemaRejectsUnknownAndDuplicateFields) {
    TestSignature signature;
    const char *unknown =
        "{\"schema_version\":1,\"expression\":true,\"extra\":0}";
    const char *duplicate =
        "{\"schema_version\":1,\"schema_version\":1,\"expression\":true}";
    ql_precondition *precondition = nullptr;
    ql_error error{};

    EXPECT_EQ(QL_STATUS_SCHEMA_MISMATCH,
              parse(signature.view, unknown, &precondition, &error));
    EXPECT_EQ(QL_STATUS_SCHEMA_MISMATCH,
              parse(signature.view, duplicate, &precondition, &error));
    EXPECT_EQ(nullptr, precondition);
}

TEST(Precondition, SignatureValidationRejectsBadWidthsBeforeParsing) {
    TestSignature signature;
    ql_precondition *precondition = nullptr;
    ql_error error{};

    signature.arguments[2].bit_width = 32u;
    EXPECT_EQ(QL_STATUS_INVALID_ARGUMENT,
              parse(signature.view, nullptr, &precondition, &error));
    EXPECT_EQ(nullptr, precondition);

    signature.arguments[2].bit_width = 64u;
    signature.arguments[5].bit_width = 8u;
    EXPECT_EQ(QL_STATUS_INVALID_ARGUMENT,
              parse(signature.view, nullptr, &precondition, &error));
    EXPECT_EQ(nullptr, precondition);

    signature.arguments[5].bit_width = 1u;
    signature.view.pointer_width = 7u;
    EXPECT_EQ(QL_STATUS_INVALID_ARGUMENT,
              parse(signature.view, nullptr, &precondition, &error));
    EXPECT_EQ(nullptr, precondition);
}

}  // namespace
