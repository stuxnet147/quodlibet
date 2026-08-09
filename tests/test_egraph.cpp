#include "quodlibet/egraph.h"

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <functional>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <type_traits>
#include <vector>

#include <gtest/gtest.h>

namespace {

struct EGraphDeleter {
    void operator()(ql_egraph *graph) const { ql_egraph_destroy(graph); }
};

using EGraphPtr = std::unique_ptr<ql_egraph, EGraphDeleter>;

EGraphPtr make_graph(const ql_egraph_config_v1 *config = nullptr) {
    ql_egraph *raw = nullptr;
    ql_error error{};
    EXPECT_EQ(QL_STATUS_OK,
              ql_egraph_create(config, nullptr, &raw, &error))
        << error.message;
    return EGraphPtr(raw);
}

ql_egraph_term_id bv_variable(ql_egraph *graph, const char *name,
                              std::uint32_t width = 32u) {
    ql_egraph_term_id term = QL_EGRAPH_INVALID_TERM;
    ql_error error{};
    EXPECT_EQ(QL_STATUS_OK,
              ql_egraph_make_bv_variable(graph, name, width, &term, &error))
        << error.message;
    return term;
}

ql_egraph_term_id bv_constant(ql_egraph *graph, std::uint64_t value,
                              std::uint32_t width = 32u) {
    ql_egraph_term_id term = QL_EGRAPH_INVALID_TERM;
    ql_error error{};
    EXPECT_EQ(QL_STATUS_OK,
              ql_egraph_make_bv_u64(graph, width, value, &term, &error))
        << error.message;
    return term;
}

ql_egraph_term_id operation(ql_egraph *graph, ql_egraph_operator op,
                            std::initializer_list<ql_egraph_term_id> args) {
    const std::vector<ql_egraph_term_id> operands(args);
    ql_egraph_term_id term = QL_EGRAPH_INVALID_TERM;
    ql_error error{};
    EXPECT_EQ(QL_STATUS_OK,
              ql_egraph_make_operation(graph, op, operands.data(),
                                       operands.size(), &term, &error))
        << error.message;
    return term;
}

struct EvalValue {
    ql_egraph_sort_kind kind{};
    std::uint32_t width{};
    std::uint64_t bits{};
};

using Assignment = std::map<std::string, EvalValue>;

std::uint64_t width_mask(std::uint32_t width) {
    return width == 64u ? UINT64_MAX
                        : (UINT64_C(1) << width) - UINT64_C(1);
}

EvalValue evaluate_term(
    ql_egraph *graph, ql_egraph_term_id term,
    const Assignment &assignment,
    std::map<ql_egraph_term_id, EvalValue> *memo) {
    const auto cached = memo->find(term);
    if (cached != memo->end()) {
        return cached->second;
    }
    ql_egraph_term_view_v1 view{};
    ql_error error{};
    if (ql_egraph_get_term(graph, term, &view, &error) != QL_STATUS_OK) {
        ADD_FAILURE() << error.message;
        return {};
    }
    EvalValue value{view.type.kind, view.type.bit_width, 0u};
    std::vector<EvalValue> operands;
    for (std::size_t index = 0u; index < view.operand_count; ++index) {
        operands.push_back(
            evaluate_term(graph, view.operands[index], assignment, memo));
    }
    switch (view.op) {
    case QL_EGRAPH_OP_VARIABLE: {
        const auto found = assignment.find(view.symbol);
        if (found == assignment.end()) {
            ADD_FAILURE() << "missing value for " << view.symbol;
            return {};
        }
        value = found->second;
        break;
    }
    case QL_EGRAPH_OP_BOOL_CONSTANT:
        value.bits = view.constant_le[0] & 1u;
        break;
    case QL_EGRAPH_OP_BV_CONSTANT:
        for (std::size_t index = 0u; index < view.constant_size; ++index) {
            value.bits |= static_cast<std::uint64_t>(view.constant_le[index])
                          << (index * 8u);
        }
        value.bits &= width_mask(value.width);
        break;
    case QL_EGRAPH_OP_BOOL_NOT:
        value.bits = operands[0].bits ^ 1u;
        break;
    case QL_EGRAPH_OP_BOOL_AND:
        value.bits = operands[0].bits & operands[1].bits;
        break;
    case QL_EGRAPH_OP_BOOL_OR:
        value.bits = operands[0].bits | operands[1].bits;
        break;
    case QL_EGRAPH_OP_BOOL_XOR:
        value.bits = operands[0].bits ^ operands[1].bits;
        break;
    case QL_EGRAPH_OP_BV_NOT:
        value.bits = ~operands[0].bits & width_mask(value.width);
        break;
    case QL_EGRAPH_OP_BV_AND:
        value.bits = operands[0].bits & operands[1].bits;
        break;
    case QL_EGRAPH_OP_BV_OR:
        value.bits = operands[0].bits | operands[1].bits;
        break;
    case QL_EGRAPH_OP_BV_XOR:
        value.bits = operands[0].bits ^ operands[1].bits;
        break;
    case QL_EGRAPH_OP_BV_ADD:
        value.bits =
            (operands[0].bits + operands[1].bits) & width_mask(value.width);
        break;
    case QL_EGRAPH_OP_BV_SUB:
        value.bits =
            (operands[0].bits - operands[1].bits) & width_mask(value.width);
        break;
    case QL_EGRAPH_OP_BV_MUL:
        value.bits =
            (operands[0].bits * operands[1].bits) & width_mask(value.width);
        break;
    case QL_EGRAPH_OP_EQUAL:
        value.kind = QL_EGRAPH_SORT_BOOL;
        value.width = 1u;
        value.bits = operands[0].kind == operands[1].kind &&
                             operands[0].width == operands[1].width &&
                             operands[0].bits == operands[1].bits
                         ? 1u
                         : 0u;
        break;
    case QL_EGRAPH_OP_ITE:
        value = operands[0].bits != 0u ? operands[1] : operands[2];
        break;
    default:
        ADD_FAILURE() << "unexpected operator " << view.op;
        return {};
    }
    (*memo)[term] = value;
    return value;
}

void collect_variables(ql_egraph *graph, ql_egraph_term_id term,
                       std::map<std::string, ql_egraph_type> *variables,
                       std::set<ql_egraph_term_id> *visited) {
    if (!visited->insert(term).second) {
        return;
    }
    ql_egraph_term_view_v1 view{};
    ql_error error{};
    if (ql_egraph_get_term(graph, term, &view, &error) != QL_STATUS_OK) {
        ADD_FAILURE() << error.message;
        return;
    }
    if (view.op == QL_EGRAPH_OP_VARIABLE) {
        (*variables)[view.symbol] = view.type;
    }
    for (std::size_t index = 0u; index < view.operand_count; ++index) {
        collect_variables(graph, view.operands[index], variables, visited);
    }
}

void expect_rewrite_exhaustively_valid(
    ql_egraph *graph, const ql_egraph_merge_record_v1 &record) {
    std::map<std::string, ql_egraph_type> variable_map;
    std::set<ql_egraph_term_id> visited;
    collect_variables(graph, record.lhs_term, &variable_map, &visited);
    collect_variables(graph, record.rhs_term, &variable_map, &visited);
    const std::vector<std::pair<std::string, ql_egraph_type>> variables(
        variable_map.begin(), variable_map.end());
    Assignment assignment;
    std::function<void(std::size_t)> enumerate = [&](std::size_t index) {
        if (index == variables.size()) {
            std::map<ql_egraph_term_id, EvalValue> memo;
            const EvalValue lhs = evaluate_term(
                graph, record.lhs_term, assignment, &memo);
            memo.clear();
            const EvalValue rhs = evaluate_term(
                graph, record.rhs_term, assignment, &memo);
            EXPECT_EQ(lhs.kind, rhs.kind) << record.reason;
            EXPECT_EQ(lhs.width, rhs.width) << record.reason;
            EXPECT_EQ(lhs.bits, rhs.bits) << record.reason;
            return;
        }
        const auto &[name, type] = variables[index];
        const std::uint64_t cardinality =
            type.kind == QL_EGRAPH_SORT_BOOL
                ? 2u
                : UINT64_C(1) << type.bit_width;
        for (std::uint64_t value = 0u; value < cardinality; ++value) {
            assignment[name] = EvalValue{type.kind, type.bit_width, value};
            enumerate(index + 1u);
        }
    };
    enumerate(0u);
}

void populate_rewrite_patterns(ql_egraph *graph, std::uint32_t width) {
    ql_egraph_term_id truth{};
    ql_egraph_term_id falsity{};
    ql_error error{};
    ASSERT_EQ(QL_STATUS_OK,
              ql_egraph_make_bool_constant(graph, 1u, &truth, &error));
    ASSERT_EQ(QL_STATUS_OK,
              ql_egraph_make_bool_constant(graph, 0u, &falsity, &error));
    ql_egraph_term_id p{};
    ql_egraph_term_id q{};
    ASSERT_EQ(QL_STATUS_OK,
              ql_egraph_make_bool_variable(graph, "p", &p, &error));
    ASSERT_EQ(QL_STATUS_OK,
              ql_egraph_make_bool_variable(graph, "q", &q, &error));

    const ql_egraph_term_id not_p =
        operation(graph, QL_EGRAPH_OP_BOOL_NOT, {p});
    operation(graph, QL_EGRAPH_OP_BOOL_NOT, {not_p});
    operation(graph, QL_EGRAPH_OP_BOOL_NOT, {truth});
    operation(graph, QL_EGRAPH_OP_BOOL_NOT, {falsity});
    operation(graph, QL_EGRAPH_OP_BOOL_AND, {p, p});
    operation(graph, QL_EGRAPH_OP_BOOL_AND, {truth, p});
    operation(graph, QL_EGRAPH_OP_BOOL_AND, {falsity, p});
    operation(graph, QL_EGRAPH_OP_BOOL_AND, {p, q});
    operation(graph, QL_EGRAPH_OP_BOOL_OR, {p, p});
    operation(graph, QL_EGRAPH_OP_BOOL_OR, {falsity, p});
    operation(graph, QL_EGRAPH_OP_BOOL_OR, {truth, p});
    operation(graph, QL_EGRAPH_OP_BOOL_OR, {p, q});
    operation(graph, QL_EGRAPH_OP_BOOL_XOR, {p, p});
    operation(graph, QL_EGRAPH_OP_BOOL_XOR, {falsity, p});
    operation(graph, QL_EGRAPH_OP_BOOL_XOR, {p, q});

    const ql_egraph_term_id x = bv_variable(graph, "x", width);
    const ql_egraph_term_id y = bv_variable(graph, "y", width);
    const ql_egraph_term_id zero = bv_constant(graph, 0u, width);
    const ql_egraph_term_id one = bv_constant(graph, 1u, width);
    const ql_egraph_term_id ones =
        bv_constant(graph, width_mask(width), width);
    const ql_egraph_term_id not_x =
        operation(graph, QL_EGRAPH_OP_BV_NOT, {x});
    operation(graph, QL_EGRAPH_OP_BV_NOT, {not_x});
    operation(graph, QL_EGRAPH_OP_BV_AND, {x, x});
    operation(graph, QL_EGRAPH_OP_BV_AND, {zero, x});
    operation(graph, QL_EGRAPH_OP_BV_AND, {ones, x});
    operation(graph, QL_EGRAPH_OP_BV_AND, {x, y});
    operation(graph, QL_EGRAPH_OP_BV_OR, {x, x});
    operation(graph, QL_EGRAPH_OP_BV_OR, {zero, x});
    operation(graph, QL_EGRAPH_OP_BV_OR, {ones, x});
    operation(graph, QL_EGRAPH_OP_BV_OR, {x, y});
    operation(graph, QL_EGRAPH_OP_BV_XOR, {x, x});
    operation(graph, QL_EGRAPH_OP_BV_XOR, {zero, x});
    operation(graph, QL_EGRAPH_OP_BV_XOR, {x, y});
    operation(graph, QL_EGRAPH_OP_BV_ADD, {zero, x});
    operation(graph, QL_EGRAPH_OP_BV_ADD, {x, y});
    operation(graph, QL_EGRAPH_OP_BV_SUB, {x, x});
    operation(graph, QL_EGRAPH_OP_BV_SUB, {x, zero});
    operation(graph, QL_EGRAPH_OP_BV_MUL, {zero, x});
    operation(graph, QL_EGRAPH_OP_BV_MUL, {one, x});
    operation(graph, QL_EGRAPH_OP_BV_MUL, {x, y});
    operation(graph, QL_EGRAPH_OP_EQUAL, {x, x});
    operation(graph, QL_EGRAPH_OP_EQUAL, {x, y});
    operation(graph, QL_EGRAPH_OP_ITE, {truth, x, y});
    operation(graph, QL_EGRAPH_OP_ITE, {falsity, x, y});
    operation(graph, QL_EGRAPH_OP_ITE, {p, x, x});
}

TEST(EGraphABI, UsesOpaqueIdsAndFixedEvidenceRecords) {
    static_assert(std::is_standard_layout_v<ql_egraph_config_v1>);
    static_assert(std::is_standard_layout_v<ql_egraph_merge_record_v1>);
    static_assert(sizeof(ql_egraph_term_id) == sizeof(std::uint32_t));
    static_assert(sizeof(ql_egraph_class_id) == sizeof(std::uint32_t));
    static_assert(offsetof(ql_egraph_config_v1, struct_size) == 0u);
    static_assert(QL_EGRAPH_MAX_ARITY == 3u);
    static_assert(sizeof(ql_egraph_merge_record_v1::reason) ==
                  QL_EGRAPH_REASON_CAPACITY);

    EXPECT_EQ(0u, QL_EGRAPH_INVALID_TERM);
    EXPECT_EQ(0u, QL_EGRAPH_VERDICT_UNKNOWN);
    EXPECT_EQ(1u, QL_EGRAPH_VERDICT_PROVED_EQUAL);
}

TEST(EGraphTerms, HashConsesTypedLeavesConstantsAndOperations) {
    const auto graph = make_graph();
    ql_egraph_term_id x1{};
    ql_egraph_term_id x2{};
    ql_egraph_term_id zero1{};
    ql_egraph_term_id zero2{};
    ql_egraph_term_id add1{};
    ql_egraph_term_id add2{};
    ql_egraph_term_view_v1 view{};
    ql_error error{};

    ASSERT_NE(nullptr, graph);
    ASSERT_EQ(QL_STATUS_OK,
              ql_egraph_make_bv_variable(graph.get(), "x", 32u, &x1,
                                         &error));
    ASSERT_EQ(QL_STATUS_OK,
              ql_egraph_make_bv_variable(graph.get(), "x", 32u, &x2,
                                         &error));
    ASSERT_EQ(QL_STATUS_OK,
              ql_egraph_make_bv_u64(graph.get(), 32u, 0u, &zero1, &error));
    ASSERT_EQ(QL_STATUS_OK,
              ql_egraph_make_bv_u64(graph.get(), 32u, 0u, &zero2, &error));
    const ql_egraph_term_id operands[] = {x1, zero1};
    ASSERT_EQ(QL_STATUS_OK,
              ql_egraph_make_operation(graph.get(), QL_EGRAPH_OP_BV_ADD,
                                       operands, 2u, &add1, &error));
    ASSERT_EQ(QL_STATUS_OK,
              ql_egraph_make_operation(graph.get(), QL_EGRAPH_OP_BV_ADD,
                                       operands, 2u, &add2, &error));

    EXPECT_EQ(x1, x2);
    EXPECT_EQ(zero1, zero2);
    EXPECT_EQ(add1, add2);
    EXPECT_EQ(3u, ql_egraph_term_count(graph.get()));
    ASSERT_EQ(QL_STATUS_OK,
              ql_egraph_get_term(graph.get(), add1, &view, &error));
    EXPECT_EQ(QL_EGRAPH_SORT_BITVECTOR, view.type.kind);
    EXPECT_EQ(32u, view.type.bit_width);
    EXPECT_EQ(QL_EGRAPH_OP_BV_ADD, view.op);
    ASSERT_EQ(2u, view.operand_count);
    EXPECT_EQ(x1, view.operands[0]);
    EXPECT_EQ(zero1, view.operands[1]);
}

TEST(EGraphTerms, RejectsIllTypedAndSemanticallyUnsafeOperators) {
    const auto graph = make_graph();
    ql_egraph_term_id bit = bv_variable(graph.get(), "x");
    ql_egraph_term_id truth{};
    ql_egraph_term_id output{};
    ql_error error{};

    ASSERT_NE(nullptr, graph);
    ASSERT_EQ(QL_STATUS_OK,
              ql_egraph_make_bool_constant(graph.get(), 1u, &truth, &error));
    const ql_egraph_term_id mixed[] = {truth, bit};
    EXPECT_EQ(QL_STATUS_TYPE_MISMATCH,
              ql_egraph_make_operation(graph.get(), QL_EGRAPH_OP_BOOL_AND,
                                       mixed, 2u, &output, &error));

    constexpr ql_egraph_operator rejected[] = {
        QL_EGRAPH_OP_BV_UDIV,
        QL_EGRAPH_OP_BV_SDIV,
        QL_EGRAPH_OP_BV_UREM,
        QL_EGRAPH_OP_BV_SREM,
        QL_EGRAPH_OP_BV_SHL,
        QL_EGRAPH_OP_BV_LSHR,
        QL_EGRAPH_OP_BV_ASHR,
        QL_EGRAPH_OP_CHECKED_ARITHMETIC,
        QL_EGRAPH_OP_UNDEFINED_BEHAVIOUR,
        QL_EGRAPH_OP_MEMORY,
        QL_EGRAPH_OP_EFFECT_SEQUENCE,
    };
    const ql_egraph_term_id binary[] = {bit, bit};
    for (const ql_egraph_operator op : rejected) {
        EXPECT_EQ(0u, ql_egraph_operator_is_supported(op));
        EXPECT_EQ(QL_STATUS_INVALID_ARGUMENT,
                  ql_egraph_make_operation(graph.get(), op, binary, 2u,
                                           &output, &error))
            << "operator=" << op;
    }

    const std::uint8_t noncanonical[] = {0xffu};
    EXPECT_EQ(QL_STATUS_INVALID_ARGUMENT,
              ql_egraph_make_bv_constant(graph.get(), 3u, noncanonical, 1u,
                                         &output, &error));
}

TEST(EGraphCongruence, ClosesAxiomsAndRecordsReconstructibleReasons) {
    const auto graph = make_graph();
    const ql_egraph_term_id a = bv_variable(graph.get(), "a");
    const ql_egraph_term_id b = bv_variable(graph.get(), "b");
    const ql_egraph_term_id not_a =
        operation(graph.get(), QL_EGRAPH_OP_BV_NOT, {a});
    const ql_egraph_term_id not_b =
        operation(graph.get(), QL_EGRAPH_OP_BV_NOT, {b});
    ql_egraph_saturation_result_v1 saturation{};
    ql_egraph_evidence_view_v1 evidence{};
    ql_egraph_term_view_v1 left_view{};
    ql_egraph_term_view_v1 right_view{};
    ql_error error{};

    ASSERT_NE(nullptr, graph);
    ASSERT_EQ(QL_STATUS_OK,
              ql_egraph_assume_equal(graph.get(), a, b,
                                     "input.same-value", &error))
        << error.message;
    ASSERT_EQ(QL_STATUS_OK,
              ql_egraph_saturate(graph.get(), nullptr, &saturation, &error))
        << error.message;
    ASSERT_TRUE(saturation.complete);
    ASSERT_EQ(QL_STATUS_OK,
              ql_egraph_get_term(graph.get(), not_a, &left_view, &error));
    ASSERT_EQ(QL_STATUS_OK,
              ql_egraph_get_term(graph.get(), not_b, &right_view, &error));
    EXPECT_EQ(left_view.current_class, right_view.current_class);

    ASSERT_EQ(QL_STATUS_OK,
              ql_egraph_evidence(graph.get(), &evidence, &error));
    ASSERT_GE(evidence.count, 2u);
    EXPECT_EQ(1u, evidence.records[0].sequence);
    EXPECT_EQ(QL_EGRAPH_MERGE_AXIOM, evidence.records[0].kind);
    EXPECT_STREQ("input.same-value", evidence.records[0].reason);
    EXPECT_EQ(a, evidence.records[0].lhs_term);
    EXPECT_EQ(b, evidence.records[0].rhs_term);

    bool saw_congruence = false;
    for (std::size_t index = 0u; index < evidence.count; ++index) {
        const auto &record = evidence.records[index];
        EXPECT_EQ(index + 1u, record.sequence);
        if (record.kind == QL_EGRAPH_MERGE_CONGRUENCE &&
            ((record.lhs_term == not_a && record.rhs_term == not_b) ||
             (record.lhs_term == not_b && record.rhs_term == not_a))) {
            saw_congruence = true;
            EXPECT_STREQ("congruence", record.reason);
            ASSERT_EQ(1u, record.lhs_operand_count);
            ASSERT_EQ(1u, record.rhs_operand_count);
            EXPECT_EQ(record.lhs_operands[0], record.rhs_operands[0]);
        }
    }
    EXPECT_TRUE(saw_congruence);
}

TEST(EGraphRewrites, ProvesOnlyAfterCompleteSafeSaturationAndExtracts) {
    const auto graph = make_graph();
    const ql_egraph_term_id x = bv_variable(graph.get(), "x");
    const ql_egraph_term_id zero = bv_constant(graph.get(), 0u);
    const ql_egraph_term_id ones = bv_constant(graph.get(), 0xffffffffu);
    const ql_egraph_term_id xor_self =
        operation(graph.get(), QL_EGRAPH_OP_BV_XOR, {x, x});
    const ql_egraph_term_id and_ones =
        operation(graph.get(), QL_EGRAPH_OP_BV_AND, {x, ones});
    const ql_egraph_term_id add_zero =
        operation(graph.get(), QL_EGRAPH_OP_BV_ADD, {x, zero});
    ql_egraph_proof_result_v1 proof{};
    ql_egraph_term_id extracted{};
    std::uint64_t cost{};
    ql_egraph_evidence_view_v1 evidence{};
    ql_error error{};

    ASSERT_NE(nullptr, graph);
    ASSERT_EQ(QL_STATUS_OK,
              ql_egraph_prove_equal(graph.get(), xor_self, zero, nullptr,
                                    &proof, &error))
        << error.message;
    EXPECT_TRUE(proof.saturation.complete);
    EXPECT_EQ(QL_EGRAPH_VERDICT_PROVED_EQUAL, proof.verdict);

    ASSERT_EQ(QL_STATUS_OK,
              ql_egraph_prove_equal(graph.get(), and_ones, x, nullptr,
                                    &proof, &error));
    EXPECT_EQ(QL_EGRAPH_VERDICT_PROVED_EQUAL, proof.verdict);
    ASSERT_EQ(QL_STATUS_OK,
              ql_egraph_prove_equal(graph.get(), add_zero, x, nullptr,
                                    &proof, &error));
    EXPECT_EQ(QL_EGRAPH_VERDICT_PROVED_EQUAL, proof.verdict);

    ASSERT_EQ(QL_STATUS_OK,
              ql_egraph_extract(graph.get(), add_zero, &extracted, &cost,
                                &error));
    EXPECT_EQ(x, extracted);
    EXPECT_EQ(1u, cost);

    ASSERT_EQ(QL_STATUS_OK,
              ql_egraph_evidence(graph.get(), &evidence, &error));
    bool saw_xor = false;
    bool saw_and = false;
    bool saw_add = false;
    for (std::size_t index = 0u; index < evidence.count; ++index) {
        saw_xor |= std::strcmp(evidence.records[index].reason,
                               "bv.xor.self") == 0;
        saw_and |= std::strcmp(evidence.records[index].reason,
                               "bv.and.ones") == 0;
        saw_add |= std::strcmp(evidence.records[index].reason,
                               "bv.add.zero") == 0;
    }
    EXPECT_TRUE(saw_xor);
    EXPECT_TRUE(saw_and);
    EXPECT_TRUE(saw_add);
}

TEST(EGraphRewrites, EveryBuiltinRuleIsExhaustiveForWidthsOneThroughFour) {
    const std::set<std::string> expected_reasons = {
        "bool.not.involution",
        "bool.not.true",
        "bool.not.false",
        "bool.xor.self",
        "bool.idempotent",
        "bool.and.true",
        "bool.and.false",
        "bool.and.commutative",
        "bool.or.false",
        "bool.or.true",
        "bool.or.commutative",
        "bool.xor.false",
        "bool.xor.commutative",
        "bv.not.involution",
        "bv.xor.self",
        "bv.sub.self",
        "bv.idempotent",
        "bv.and.zero",
        "bv.and.ones",
        "bv.and.commutative",
        "bv.or.zero",
        "bv.or.ones",
        "bv.or.commutative",
        "bv.xor.zero",
        "bv.xor.commutative",
        "bv.add.zero",
        "bv.add.commutative",
        "bv.sub.zero",
        "bv.mul.zero",
        "bv.mul.one",
        "bv.mul.commutative",
        "equal.reflexive",
        "equal.symmetric",
        "ite.true",
        "ite.false",
        "ite.same",
    };
    std::set<std::string> observed_reasons;

    for (std::uint32_t width = 1u; width <= 4u; ++width) {
        const auto graph = make_graph();
        ql_egraph_saturation_result_v1 saturation{};
        ql_egraph_evidence_view_v1 evidence{};
        ql_error error{};
        ASSERT_NE(nullptr, graph);
        populate_rewrite_patterns(graph.get(), width);
        ASSERT_EQ(QL_STATUS_OK,
                  ql_egraph_saturate(graph.get(), nullptr, &saturation,
                                     &error))
            << error.message;
        ASSERT_TRUE(saturation.complete) << "width=" << width;
        ASSERT_EQ(QL_STATUS_OK,
                  ql_egraph_evidence(graph.get(), &evidence, &error));
        for (std::size_t index = 0u; index < evidence.count; ++index) {
            const auto &record = evidence.records[index];
            if (record.kind != QL_EGRAPH_MERGE_REWRITE) {
                continue;
            }
            observed_reasons.insert(record.reason);
            expect_rewrite_exhaustively_valid(graph.get(), record);
        }
    }
    EXPECT_EQ(expected_reasons, observed_reasons);
}

TEST(EGraphVerdict, IncompleteSaturationStaysUnknownEvenWhenRootsMerge) {
    const auto graph = make_graph();
    const ql_egraph_term_id x = bv_variable(graph.get(), "x");
    const ql_egraph_term_id zero = bv_constant(graph.get(), 0u);
    const ql_egraph_term_id add_zero =
        operation(graph.get(), QL_EGRAPH_OP_BV_ADD, {x, zero});
    ql_egraph_saturation_limits_v1 limits{};
    ql_egraph_proof_result_v1 proof{};
    ql_egraph_term_view_v1 lhs_view{};
    ql_egraph_term_view_v1 rhs_view{};
    ql_error error{};

    ql_egraph_saturation_limits_init(&limits);
    limits.max_iterations = 1u;
    ASSERT_EQ(QL_STATUS_OK,
              ql_egraph_prove_equal(graph.get(), add_zero, x, &limits,
                                    &proof, &error));
    EXPECT_FALSE(proof.saturation.complete);
    EXPECT_EQ(QL_EGRAPH_STOP_ITERATION_LIMIT,
              proof.saturation.stop_reason);
    EXPECT_EQ(QL_EGRAPH_VERDICT_UNKNOWN, proof.verdict);

    ASSERT_EQ(QL_STATUS_OK,
              ql_egraph_get_term(graph.get(), add_zero, &lhs_view, &error));
    ASSERT_EQ(QL_STATUS_OK,
              ql_egraph_get_term(graph.get(), x, &rhs_view, &error));
    EXPECT_EQ(lhs_view.current_class, rhs_view.current_class);
}

TEST(EGraphVerdict, DistinctSaturatedRootsAreUnknownNotDisproved) {
    const auto graph = make_graph();
    const ql_egraph_term_id x = bv_variable(graph.get(), "x");
    const ql_egraph_term_id y = bv_variable(graph.get(), "y");
    ql_egraph_proof_result_v1 proof{};
    ql_error error{};

    ASSERT_EQ(QL_STATUS_OK,
              ql_egraph_prove_equal(graph.get(), x, y, nullptr, &proof,
                                    &error));
    EXPECT_TRUE(proof.saturation.complete);
    EXPECT_EQ(QL_EGRAPH_VERDICT_UNKNOWN, proof.verdict);
}

TEST(EGraphVerdict, PublicAxiomsMakeProofExplicitlyAssumptionRelative) {
    const auto graph = make_graph();
    const ql_egraph_term_id x = bv_variable(graph.get(), "x");
    const ql_egraph_term_id y = bv_variable(graph.get(), "y");
    ql_egraph_proof_result_v1 proof{};
    ql_egraph_evidence_view_v1 evidence{};
    ql_error error{};

    ASSERT_EQ(QL_STATUS_OK,
              ql_egraph_prove_equal(graph.get(), x, y, nullptr, &proof,
                                    &error));
    ASSERT_EQ(QL_EGRAPH_VERDICT_UNKNOWN, proof.verdict);
    ASSERT_FALSE(proof.depends_on_axioms);

    ASSERT_EQ(QL_STATUS_OK,
              ql_egraph_assume_equal(graph.get(), x, y,
                                     "caller.precondition", &error));
    ASSERT_EQ(QL_STATUS_OK,
              ql_egraph_prove_equal(graph.get(), x, y, nullptr, &proof,
                                    &error));
    EXPECT_EQ(QL_EGRAPH_VERDICT_PROVED_EQUAL, proof.verdict);
    EXPECT_TRUE(proof.depends_on_axioms);
    ASSERT_EQ(QL_STATUS_OK,
              ql_egraph_evidence(graph.get(), &evidence, &error));
    ASSERT_FALSE(evidence.count == 0u);
    EXPECT_EQ(QL_EGRAPH_MERGE_AXIOM, evidence.records[0].kind);
    EXPECT_STREQ("caller.precondition", evidence.records[0].reason);
}

TEST(EGraphResources, TermLimitProducesUnknownWithoutInventingAProof) {
    ql_egraph_config_v1 config{};
    ql_egraph_saturation_result_v1 saturation{};
    ql_egraph_proof_result_v1 proof{};
    ql_error error{};
    ql_egraph_config_init(&config);
    config.max_terms = 2u;
    config.max_classes = 2u;
    const auto graph = make_graph(&config);
    const ql_egraph_term_id x = bv_variable(graph.get(), "x");
    const ql_egraph_term_id xor_self =
        operation(graph.get(), QL_EGRAPH_OP_BV_XOR, {x, x});

    ASSERT_EQ(QL_STATUS_OK,
              ql_egraph_saturate(graph.get(), nullptr, &saturation, &error));
    EXPECT_FALSE(saturation.complete);
    EXPECT_EQ(QL_EGRAPH_STOP_TERM_LIMIT, saturation.stop_reason);

    ASSERT_EQ(QL_STATUS_OK,
              ql_egraph_prove_equal(graph.get(), xor_self, x, nullptr,
                                    &proof, &error));
    EXPECT_FALSE(proof.saturation.complete);
    EXPECT_EQ(QL_EGRAPH_VERDICT_UNKNOWN, proof.verdict);
}

TEST(EGraphResources, RewriteAndMergeLimitsNeverProduceProofs) {
    {
        const auto graph = make_graph();
        const ql_egraph_term_id x = bv_variable(graph.get(), "x");
        const ql_egraph_term_id zero = bv_constant(graph.get(), 0u);
        const ql_egraph_term_id add_zero =
            operation(graph.get(), QL_EGRAPH_OP_BV_ADD, {x, zero});
        ql_egraph_saturation_limits_v1 limits{};
        ql_egraph_proof_result_v1 proof{};
        ql_error error{};
        ql_egraph_saturation_limits_init(&limits);
        limits.max_rewrite_applications = 1u;
        ASSERT_EQ(QL_STATUS_OK,
                  ql_egraph_prove_equal(graph.get(), add_zero, x, &limits,
                                        &proof, &error));
        EXPECT_FALSE(proof.saturation.complete);
        EXPECT_EQ(QL_EGRAPH_STOP_REWRITE_LIMIT,
                  proof.saturation.stop_reason);
        EXPECT_EQ(QL_EGRAPH_VERDICT_UNKNOWN, proof.verdict);
    }
    {
        ql_egraph_config_v1 config{};
        ql_egraph_config_init(&config);
        config.max_merges = 1u;
        const auto graph = make_graph(&config);
        const ql_egraph_term_id a = bv_variable(graph.get(), "a");
        const ql_egraph_term_id b = bv_variable(graph.get(), "b");
        const ql_egraph_term_id not_a =
            operation(graph.get(), QL_EGRAPH_OP_BV_NOT, {a});
        const ql_egraph_term_id not_b =
            operation(graph.get(), QL_EGRAPH_OP_BV_NOT, {b});
        ql_egraph_proof_result_v1 proof{};
        ql_error error{};
        ASSERT_EQ(QL_STATUS_OK,
                  ql_egraph_assume_equal(graph.get(), a, b,
                                         "input.same", &error));
        ASSERT_EQ(QL_STATUS_OK,
                  ql_egraph_prove_equal(graph.get(), not_a, not_b, nullptr,
                                        &proof, &error));
        EXPECT_FALSE(proof.saturation.complete);
        EXPECT_EQ(QL_EGRAPH_STOP_MERGE_LIMIT,
                  proof.saturation.stop_reason);
        EXPECT_EQ(QL_EGRAPH_VERDICT_UNKNOWN, proof.verdict);
    }
}

}  // namespace
