#include "quodlibet/egraph.h"

#include <cstdint>
#include <memory>
#include <set>
#include <string>
#include <vector>

#include <gtest/gtest.h>

namespace {

struct EGraphDeleter {
    void operator()(ql_egraph *graph) const { ql_egraph_destroy(graph); }
};

using EGraphPtr = std::unique_ptr<ql_egraph, EGraphDeleter>;

EGraphPtr make_graph() {
    ql_egraph *raw = nullptr;
    ql_error error{};
    EXPECT_EQ(QL_STATUS_OK, ql_egraph_create(nullptr, nullptr, &raw, &error))
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

ql_egraph_term_id bool_variable(ql_egraph *graph, const char *name) {
    ql_egraph_term_id term = QL_EGRAPH_INVALID_TERM;
    ql_error error{};
    EXPECT_EQ(QL_STATUS_OK,
              ql_egraph_make_bool_variable(graph, name, &term, &error))
        << error.message;
    return term;
}

ql_egraph_term_id bool_constant(ql_egraph *graph, std::uint32_t value) {
    ql_egraph_term_id term = QL_EGRAPH_INVALID_TERM;
    ql_error error{};
    EXPECT_EQ(QL_STATUS_OK,
              ql_egraph_make_bool_constant(graph, value, &term, &error))
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

std::vector<ql_egraph_rule_descriptor_v1> catalogue() {
    std::vector<ql_egraph_rule_descriptor_v1> rules;
    const std::uint32_t count = ql_egraph_rule_catalogue_size();
    for (std::uint32_t index = 0u; index < count; ++index) {
        ql_egraph_rule_descriptor_v1 descriptor{};
        ql_error error{};
        EXPECT_EQ(QL_STATUS_OK,
                  ql_egraph_rule_catalogue_at(index, &descriptor, &error))
            << error.message;
        rules.push_back(descriptor);
    }
    return rules;
}

ql_egraph_rule_descriptor_v1 lookup(const char *name) {
    ql_egraph_rule_descriptor_v1 descriptor{};
    ql_error error{};
    EXPECT_EQ(QL_STATUS_OK,
              ql_egraph_rule_lookup(name, &descriptor, &error))
        << name << ": " << error.message;
    return descriptor;
}

bool has(const ql_egraph_rule_descriptor_v1 &rule,
         ql_egraph_rule_conditions condition) {
    return (rule.conditions & condition) != 0u;
}

/* Saturating this graph fires bit-vector, boolean, equality, and if-then-else
   rules in one run, so the reasons it records are a wide sample of what the
   engine can emit. */
EGraphPtr saturated_sample() {
    EGraphPtr graph = make_graph();
    ql_egraph *raw = graph.get();
    const ql_egraph_term_id x = bv_variable(raw, "x");
    const ql_egraph_term_id y = bv_variable(raw, "y");
    const ql_egraph_term_id zero = bv_constant(raw, 0u);
    const ql_egraph_term_id one = bv_constant(raw, 1u);
    const ql_egraph_term_id ones = bv_constant(raw, 0xFFFFFFFFull);
    const ql_egraph_term_id p = bool_variable(raw, "p");
    const ql_egraph_term_id truth = bool_constant(raw, 1u);
    const ql_egraph_term_id falsity = bool_constant(raw, 0u);
    ql_egraph_saturation_result_v1 result{};
    ql_error error{};

    operation(raw, QL_EGRAPH_OP_BV_ADD, {x, zero});
    operation(raw, QL_EGRAPH_OP_BV_ADD, {x, y});
    operation(raw, QL_EGRAPH_OP_BV_SUB, {x, zero});
    operation(raw, QL_EGRAPH_OP_BV_SUB, {x, x});
    operation(raw, QL_EGRAPH_OP_BV_MUL, {x, zero});
    operation(raw, QL_EGRAPH_OP_BV_MUL, {x, one});
    operation(raw, QL_EGRAPH_OP_BV_AND, {x, zero});
    operation(raw, QL_EGRAPH_OP_BV_AND, {x, ones});
    operation(raw, QL_EGRAPH_OP_BV_AND, {x, x});
    operation(raw, QL_EGRAPH_OP_BV_OR, {x, zero});
    operation(raw, QL_EGRAPH_OP_BV_OR, {x, ones});
    operation(raw, QL_EGRAPH_OP_BV_XOR, {x, zero});
    operation(raw, QL_EGRAPH_OP_BV_XOR, {x, x});
    operation(raw, QL_EGRAPH_OP_BV_NOT,
              {operation(raw, QL_EGRAPH_OP_BV_NOT, {x})});
    operation(raw, QL_EGRAPH_OP_BOOL_NOT, {truth});
    operation(raw, QL_EGRAPH_OP_BOOL_NOT, {falsity});
    operation(raw, QL_EGRAPH_OP_BOOL_NOT,
              {operation(raw, QL_EGRAPH_OP_BOOL_NOT, {p})});
    operation(raw, QL_EGRAPH_OP_BOOL_AND, {p, truth});
    operation(raw, QL_EGRAPH_OP_BOOL_AND, {p, falsity});
    operation(raw, QL_EGRAPH_OP_BOOL_AND, {p, p});
    operation(raw, QL_EGRAPH_OP_BOOL_OR, {p, truth});
    operation(raw, QL_EGRAPH_OP_BOOL_OR, {p, falsity});
    operation(raw, QL_EGRAPH_OP_BOOL_XOR, {p, falsity});
    operation(raw, QL_EGRAPH_OP_BOOL_XOR, {p, p});
    operation(raw, QL_EGRAPH_OP_EQUAL, {x, x});
    operation(raw, QL_EGRAPH_OP_EQUAL, {x, y});
    operation(raw, QL_EGRAPH_OP_ITE, {truth, x, y});
    operation(raw, QL_EGRAPH_OP_ITE, {falsity, x, y});
    operation(raw, QL_EGRAPH_OP_ITE, {p, x, x});

    EXPECT_EQ(QL_STATUS_OK,
              ql_egraph_saturate(raw, nullptr, &result, &error))
        << error.message;
    return graph;
}

std::set<std::string> recorded_rewrite_reasons(const ql_egraph *graph) {
    ql_egraph_evidence_view_v1 view{};
    ql_error error{};
    std::set<std::string> reasons;
    EXPECT_EQ(QL_STATUS_OK, ql_egraph_evidence(graph, &view, &error))
        << error.message;
    for (std::size_t index = 0u; index < view.count; ++index) {
        if (view.records[index].kind == QL_EGRAPH_MERGE_REWRITE) {
            reasons.insert(view.records[index].reason);
        }
    }
    return reasons;
}

TEST(EGraphRuleCatalogue, DescribesEveryRewriteTheEngineRecords) {
    const EGraphPtr graph = saturated_sample();
    const std::set<std::string> reasons =
        recorded_rewrite_reasons(graph.get());
    ASSERT_FALSE(reasons.empty());
    for (const std::string &reason : reasons) {
        ql_egraph_rule_descriptor_v1 descriptor{};
        ql_error error{};
        EXPECT_EQ(QL_STATUS_OK,
                  ql_egraph_rule_lookup(reason.c_str(), &descriptor, &error))
            << "rewrite reason '" << reason
            << "' has no catalogue entry, so no checker can discharge it: "
            << error.message;
    }
}

TEST(EGraphRuleCatalogue, RecordedRewritesNameAnOperatorTheRuleAdmits) {
    const EGraphPtr graph = saturated_sample();
    ql_egraph_evidence_view_v1 view{};
    ql_error error{};
    ASSERT_EQ(QL_STATUS_OK,
              ql_egraph_evidence(graph.get(), &view, &error))
        << error.message;
    for (std::size_t index = 0u; index < view.count; ++index) {
        const ql_egraph_merge_record_v1 &record = view.records[index];
        if (record.kind != QL_EGRAPH_MERGE_REWRITE) {
            continue;
        }
        const ql_egraph_rule_descriptor_v1 rule = lookup(record.reason);
        ql_egraph_term_view_v1 subject{};
        ASSERT_EQ(QL_STATUS_OK,
                  ql_egraph_get_term(graph.get(), record.lhs_term, &subject,
                                     &error))
            << error.message;
        bool admitted = false;
        for (std::uint32_t slot = 0u; slot < rule.subject_op_count; ++slot) {
            admitted = admitted || rule.subject_ops[slot] == subject.op;
        }
        EXPECT_TRUE(admitted)
            << "rule " << record.reason << " fired on operator "
            << subject.op;
        EXPECT_EQ(rule.subject_arity, subject.operand_count);
    }
}

TEST(EGraphRuleCatalogue, EveryDescriptorIsInternallyConsistent) {
    const std::vector<ql_egraph_rule_descriptor_v1> rules = catalogue();
    ASSERT_FALSE(rules.empty());
    for (const ql_egraph_rule_descriptor_v1 &rule : rules) {
        SCOPED_TRACE(rule.rule_name);
        EXPECT_EQ(sizeof(ql_egraph_rule_descriptor_v1), rule.struct_size);
        EXPECT_EQ(QL_EGRAPH_RULE_CATALOGUE_VERSION, rule.catalogue_version);
        ASSERT_NE(nullptr, rule.rule_name);
        ASSERT_NE(nullptr, rule.soundness);
        EXPECT_NE('\0', rule.rule_name[0]);
        EXPECT_NE('\0', rule.soundness[0]);
        EXPECT_NE(QL_EGRAPH_RULE_SHAPE_INVALID, rule.shape);
        EXPECT_GE(rule.subject_op_count, 1u);
        EXPECT_LE(rule.subject_op_count, QL_EGRAPH_RULE_MAX_SUBJECT_OPS);
        EXPECT_GE(rule.subject_arity, 1u);
        EXPECT_LE(rule.subject_arity, QL_EGRAPH_MAX_ARITY);
        for (std::uint32_t slot = 0u; slot < rule.subject_op_count; ++slot) {
            EXPECT_TRUE(
                ql_egraph_operator_is_supported(rule.subject_ops[slot]))
                << "subject operator " << rule.subject_ops[slot];
        }

        /* A witness constant and the flag that says one is needed must agree,
           otherwise a checker either skips a premise or looks for one the
           rule never had. */
        EXPECT_EQ(has(rule, QL_EGRAPH_RULE_COND_WITNESS_CONSTANT),
                  rule.witness_constant != QL_EGRAPH_RULE_CONSTANT_NONE);
        if (has(rule, QL_EGRAPH_RULE_COND_WITNESS_CONSTANT)) {
            EXPECT_TRUE(rule.witness_operand == QL_EGRAPH_RULE_OPERAND_ANY ||
                        rule.witness_operand < rule.subject_arity);
        } else {
            EXPECT_NE(QL_EGRAPH_RULE_OPERAND_OTHER, rule.result_operand);
        }
        if (has(rule, QL_EGRAPH_RULE_COND_OPERANDS_SAME_CLASS)) {
            EXPECT_LT(rule.equal_operands[0], rule.subject_arity);
            EXPECT_LT(rule.equal_operands[1], rule.subject_arity);
            EXPECT_NE(rule.equal_operands[0], rule.equal_operands[1]);
        }
        if (rule.result_operand != QL_EGRAPH_RULE_OPERAND_NONE &&
            rule.result_operand != QL_EGRAPH_RULE_OPERAND_OTHER) {
            EXPECT_LT(rule.result_operand, rule.subject_arity);
        }
        /* Exactly one of the two ways to name a right-hand side. */
        const bool names_operand =
            rule.result_operand != QL_EGRAPH_RULE_OPERAND_NONE;
        const bool names_constant =
            rule.result_constant != QL_EGRAPH_RULE_CONSTANT_NONE;
        EXPECT_FALSE(names_operand && names_constant);
        if (rule.maximum_bit_width != 0u) {
            EXPECT_LE(rule.minimum_bit_width, rule.maximum_bit_width);
        }
    }
}

TEST(EGraphRuleCatalogue, RuleNamesAreUnique) {
    const std::vector<ql_egraph_rule_descriptor_v1> rules = catalogue();
    std::set<std::string> names;
    for (const ql_egraph_rule_descriptor_v1 &rule : rules) {
        EXPECT_TRUE(names.insert(rule.rule_name).second)
            << "duplicate rule name " << rule.rule_name;
    }
    EXPECT_EQ(rules.size(), names.size());
}

TEST(EGraphRuleCatalogue, RejectsUnknownNamesAndOutOfRangeIndices) {
    ql_egraph_rule_descriptor_v1 descriptor{};
    ql_error error{};
    EXPECT_EQ(QL_STATUS_NOT_FOUND,
              ql_egraph_rule_lookup("bv.add.associative", &descriptor,
                                    &error));
    EXPECT_NE('\0', error.message[0]);
    EXPECT_EQ(QL_STATUS_NOT_FOUND,
              ql_egraph_rule_catalogue_at(ql_egraph_rule_catalogue_size(),
                                          &descriptor, &error));
    EXPECT_EQ(QL_STATUS_INVALID_ARGUMENT,
              ql_egraph_rule_lookup(nullptr, &descriptor, &error));
    EXPECT_EQ(QL_STATUS_INVALID_ARGUMENT,
              ql_egraph_rule_lookup("bv.add.zero", nullptr, &error));
}

/* The three premise axes W6 asks for: the arithmetic model, the signedness
   reading, and the width range. An arithmetic rule that forgot to declare
   that it needs total modular arithmetic would let a frontend whose source
   language traps on overflow reuse the merge without an argument. */
TEST(EGraphRuleCatalogue, ArithmeticRulesDeclareTheTotalArithmeticPremise) {
    const char *arithmetic[] = {"bv.add.zero",       "bv.add.commutative",
                                "bv.sub.zero",       "bv.sub.self",
                                "bv.mul.zero",       "bv.mul.one",
                                "bv.mul.commutative"};
    for (const char *name : arithmetic) {
        SCOPED_TRACE(name);
        EXPECT_TRUE(
            has(lookup(name), QL_EGRAPH_RULE_COND_TOTAL_ARITHMETIC));
    }
}

TEST(EGraphRuleCatalogue, BitwiseRulesDoNotClaimAnArithmeticPremise) {
    const char *bitwise[] = {"bv.and.zero", "bv.and.ones", "bv.or.zero",
                             "bv.or.ones",  "bv.xor.zero", "bv.xor.self",
                             "bv.not.involution"};
    for (const char *name : bitwise) {
        SCOPED_TRACE(name);
        const ql_egraph_rule_descriptor_v1 rule = lookup(name);
        EXPECT_FALSE(has(rule, QL_EGRAPH_RULE_COND_TOTAL_ARITHMETIC));
        EXPECT_TRUE(has(rule, QL_EGRAPH_RULE_COND_SIGN_AGNOSTIC));
        EXPECT_TRUE(has(rule, QL_EGRAPH_RULE_COND_WIDTH_AGNOSTIC));
    }
}

TEST(EGraphRuleCatalogue, BooleanRulesArePinnedToASingleBit) {
    const std::vector<ql_egraph_rule_descriptor_v1> rules = catalogue();
    for (const ql_egraph_rule_descriptor_v1 &rule : rules) {
        if (std::string(rule.rule_name).rfind("bool.", 0u) != 0u) {
            continue;
        }
        SCOPED_TRACE(rule.rule_name);
        EXPECT_EQ(1u, rule.minimum_bit_width);
        EXPECT_EQ(1u, rule.maximum_bit_width);
        /* Propositional rules must not advertise a bit-vector premise. */
        EXPECT_FALSE(has(rule, QL_EGRAPH_RULE_COND_WIDTH_AGNOSTIC));
        EXPECT_FALSE(has(rule, QL_EGRAPH_RULE_COND_TOTAL_ARITHMETIC));
    }
}

TEST(EGraphRuleCatalogue, BitVectorRulesAcceptEveryWidth) {
    const std::vector<ql_egraph_rule_descriptor_v1> rules = catalogue();
    for (const ql_egraph_rule_descriptor_v1 &rule : rules) {
        if (std::string(rule.rule_name).rfind("bv.", 0u) != 0u) {
            continue;
        }
        SCOPED_TRACE(rule.rule_name);
        EXPECT_TRUE(has(rule, QL_EGRAPH_RULE_COND_WIDTH_AGNOSTIC));
        EXPECT_TRUE(has(rule, QL_EGRAPH_RULE_COND_SIGN_AGNOSTIC));
        EXPECT_EQ(1u, rule.minimum_bit_width);
        EXPECT_EQ(0u, rule.maximum_bit_width);
    }
}

TEST(EGraphRuleCatalogue, DigestIsDeterministicAndCoversThePremises) {
    ql_digest first{};
    ql_digest second{};
    ql_error error{};
    ASSERT_EQ(QL_STATUS_OK,
              ql_egraph_rule_catalogue_digest(&first, &error))
        << error.message;
    ASSERT_EQ(QL_STATUS_OK,
              ql_egraph_rule_catalogue_digest(&second, &error))
        << error.message;
    EXPECT_TRUE(ql_digest_equal(&first, &second));

    ql_digest zero{};
    EXPECT_FALSE(ql_digest_equal(&first, &zero));

    /* Pinning the digest is the mechanism that makes a silent premise edit a
       test failure. Changing a rule's conditions, widths, or soundness text
       without bumping QL_EGRAPH_RULE_CATALOGUE_VERSION breaks this. */
    char hex[QL_DIGEST_HEX_SIZE];
    ql_digest_hex(&first, hex);
    EXPECT_EQ(std::string(QL_EGRAPH_RULE_CATALOGUE_DIGEST_HEX),
              std::string(hex));
}

TEST(EGraphRuleCatalogue, DigestRejectsANullOutput) {
    ql_error error{};
    EXPECT_EQ(QL_STATUS_INVALID_ARGUMENT,
              ql_egraph_rule_catalogue_digest(nullptr, &error));
}

TEST(EGraphTermIdentity, InitialClassIsTheDerivationSeedNotTheCurrentRoot) {
    const EGraphPtr graph = make_graph();
    ql_egraph *raw = graph.get();
    const ql_egraph_term_id x = bv_variable(raw, "x");
    const ql_egraph_term_id y = bv_variable(raw, "y");
    ql_egraph_class_id x_seed = QL_EGRAPH_INVALID_CLASS;
    ql_egraph_class_id y_seed = QL_EGRAPH_INVALID_CLASS;
    ql_error error{};

    ASSERT_EQ(QL_STATUS_OK,
              ql_egraph_term_initial_class(raw, x, &x_seed, &error))
        << error.message;
    ASSERT_EQ(QL_STATUS_OK,
              ql_egraph_term_initial_class(raw, y, &y_seed, &error))
        << error.message;
    EXPECT_NE(x_seed, y_seed);
    EXPECT_NE(QL_EGRAPH_INVALID_CLASS, x_seed);

    ASSERT_EQ(QL_STATUS_OK,
              ql_egraph_assume_equal(raw, x, y, "test.axiom", &error))
        << error.message;

    ql_egraph_class_id x_after = QL_EGRAPH_INVALID_CLASS;
    ql_egraph_class_id y_after = QL_EGRAPH_INVALID_CLASS;
    ASSERT_EQ(QL_STATUS_OK,
              ql_egraph_term_initial_class(raw, x, &x_after, &error));
    ASSERT_EQ(QL_STATUS_OK,
              ql_egraph_term_initial_class(raw, y, &y_after, &error));
    EXPECT_EQ(x_seed, x_after);
    EXPECT_EQ(y_seed, y_after);

    /* The view's current_class has collapsed, which is exactly why a replay
       checker cannot start from it. */
    ql_egraph_term_view_v1 x_view{};
    ql_egraph_term_view_v1 y_view{};
    ASSERT_EQ(QL_STATUS_OK, ql_egraph_get_term(raw, x, &x_view, &error));
    ASSERT_EQ(QL_STATUS_OK, ql_egraph_get_term(raw, y, &y_view, &error));
    EXPECT_EQ(x_view.current_class, y_view.current_class);
}

TEST(EGraphTermIdentity, InitialClassRejectsBadArguments) {
    const EGraphPtr graph = make_graph();
    ql_egraph_class_id seed = QL_EGRAPH_INVALID_CLASS;
    ql_error error{};
    EXPECT_EQ(QL_STATUS_INVALID_ARGUMENT,
              ql_egraph_term_initial_class(nullptr, 1u, &seed, &error));
    EXPECT_EQ(QL_STATUS_INVALID_ARGUMENT,
              ql_egraph_term_initial_class(graph.get(), 1u, &seed, &error));
    const ql_egraph_term_id x = bv_variable(graph.get(), "x");
    EXPECT_EQ(QL_STATUS_INVALID_ARGUMENT,
              ql_egraph_term_initial_class(graph.get(), x, nullptr, &error));
}

}  // namespace
