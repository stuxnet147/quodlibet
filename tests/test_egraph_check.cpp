#include "quodlibet/egraph_check.h"

#include <cstdint>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

#include <gtest/gtest.h>

namespace {

struct EGraphDeleter {
    void operator()(ql_egraph *graph) const { ql_egraph_destroy(graph); }
};

struct ReportDeleter {
    void operator()(ql_egraph_check_report *report) const {
        ql_egraph_check_report_release(report);
    }
};

using EGraphPtr = std::unique_ptr<ql_egraph, EGraphDeleter>;
using ReportPtr = std::unique_ptr<ql_egraph_check_report, ReportDeleter>;

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

void saturate(ql_egraph *graph) {
    ql_egraph_saturation_result_v1 result{};
    ql_error error{};
    EXPECT_EQ(QL_STATUS_OK,
              ql_egraph_saturate(graph, nullptr, &result, &error))
        << error.message;
}

/* Every rewrite family the engine implements fires somewhere in here, so the
   log this produces is the widest sample the checker gets tested against. */
EGraphPtr wide_sample() {
    EGraphPtr graph = make_graph();
    ql_egraph *raw = graph.get();
    const ql_egraph_term_id x = bv_variable(raw, "x");
    const ql_egraph_term_id y = bv_variable(raw, "y");
    const ql_egraph_term_id zero = bv_constant(raw, 0u);
    const ql_egraph_term_id one = bv_constant(raw, 1u);
    const ql_egraph_term_id ones = bv_constant(raw, 0xFFFFFFFFull);
    const ql_egraph_term_id narrow = bv_variable(raw, "n", 12u);
    const ql_egraph_term_id narrow_ones = bv_constant(raw, 0x0FFFu, 12u);
    const ql_egraph_term_id p = bool_variable(raw, "p");
    const ql_egraph_term_id truth = bool_constant(raw, 1u);
    const ql_egraph_term_id falsity = bool_constant(raw, 0u);

    operation(raw, QL_EGRAPH_OP_BV_ADD, {x, zero});
    operation(raw, QL_EGRAPH_OP_BV_ADD, {zero, x});
    operation(raw, QL_EGRAPH_OP_BV_ADD, {x, y});
    operation(raw, QL_EGRAPH_OP_BV_SUB, {x, zero});
    operation(raw, QL_EGRAPH_OP_BV_SUB, {x, x});
    operation(raw, QL_EGRAPH_OP_BV_MUL, {x, zero});
    operation(raw, QL_EGRAPH_OP_BV_MUL, {x, one});
    operation(raw, QL_EGRAPH_OP_BV_MUL, {one, x});
    operation(raw, QL_EGRAPH_OP_BV_AND, {x, zero});
    operation(raw, QL_EGRAPH_OP_BV_AND, {x, ones});
    operation(raw, QL_EGRAPH_OP_BV_AND, {x, x});
    operation(raw, QL_EGRAPH_OP_BV_OR, {x, zero});
    operation(raw, QL_EGRAPH_OP_BV_OR, {x, ones});
    operation(raw, QL_EGRAPH_OP_BV_OR, {x, x});
    operation(raw, QL_EGRAPH_OP_BV_XOR, {x, zero});
    operation(raw, QL_EGRAPH_OP_BV_XOR, {x, x});
    operation(raw, QL_EGRAPH_OP_BV_NOT,
              {operation(raw, QL_EGRAPH_OP_BV_NOT, {x})});
    /* A narrower width exercises the masked all-ones pattern. */
    operation(raw, QL_EGRAPH_OP_BV_AND, {narrow, narrow_ones});
    operation(raw, QL_EGRAPH_OP_BV_OR, {narrow, narrow_ones});

    operation(raw, QL_EGRAPH_OP_BOOL_NOT, {truth});
    operation(raw, QL_EGRAPH_OP_BOOL_NOT, {falsity});
    operation(raw, QL_EGRAPH_OP_BOOL_NOT,
              {operation(raw, QL_EGRAPH_OP_BOOL_NOT, {p})});
    operation(raw, QL_EGRAPH_OP_BOOL_AND, {p, truth});
    operation(raw, QL_EGRAPH_OP_BOOL_AND, {truth, p});
    operation(raw, QL_EGRAPH_OP_BOOL_AND, {p, falsity});
    operation(raw, QL_EGRAPH_OP_BOOL_AND, {p, p});
    operation(raw, QL_EGRAPH_OP_BOOL_OR, {p, truth});
    operation(raw, QL_EGRAPH_OP_BOOL_OR, {p, falsity});
    operation(raw, QL_EGRAPH_OP_BOOL_OR, {p, p});
    operation(raw, QL_EGRAPH_OP_BOOL_XOR, {p, falsity});
    operation(raw, QL_EGRAPH_OP_BOOL_XOR, {p, p});

    operation(raw, QL_EGRAPH_OP_EQUAL, {x, x});
    operation(raw, QL_EGRAPH_OP_EQUAL, {x, y});
    operation(raw, QL_EGRAPH_OP_ITE, {truth, x, y});
    operation(raw, QL_EGRAPH_OP_ITE, {falsity, x, y});
    operation(raw, QL_EGRAPH_OP_ITE, {p, x, x});

    saturate(raw);
    return graph;
}

/* Mirrors what ql_egraph_check_graph does, but keeps the snapshot in the test
   so a record or a term can be corrupted before the replay. */
struct Snapshot {
    std::vector<ql_egraph_check_term_v1> terms;
    std::vector<ql_egraph_merge_record_v1> records;

    ql_egraph_check_input_v1 input() const {
        ql_egraph_check_input_v1 value{};
        ql_egraph_check_input_init(&value);
        value.terms = terms.data();
        value.term_count = terms.size();
        value.records = records.empty() ? nullptr : records.data();
        value.record_count = records.size();
        return value;
    }
};

Snapshot harvest(const ql_egraph *graph) {
    Snapshot snapshot;
    ql_egraph_evidence_view_v1 evidence{};
    ql_error error{};
    const std::uint64_t count = ql_egraph_term_count(graph);
    EXPECT_EQ(QL_STATUS_OK, ql_egraph_evidence(graph, &evidence, &error))
        << error.message;
    for (std::uint64_t index = 0u; index < count; ++index) {
        const ql_egraph_term_id term =
            static_cast<ql_egraph_term_id>(index + 1u);
        ql_egraph_term_view_v1 view{};
        ql_egraph_class_id seed = QL_EGRAPH_INVALID_CLASS;
        EXPECT_EQ(QL_STATUS_OK,
                  ql_egraph_get_term(graph, term, &view, &error))
            << error.message;
        EXPECT_EQ(QL_STATUS_OK,
                  ql_egraph_term_initial_class(graph, term, &seed, &error))
            << error.message;
        ql_egraph_check_term_v1 entry{};
        entry.struct_size = sizeof(entry);
        entry.term = term;
        entry.initial_class = seed;
        entry.type = view.type;
        entry.op = view.op;
        entry.operand_count = view.operand_count;
        for (std::size_t slot = 0u; slot < QL_EGRAPH_MAX_ARITY; ++slot) {
            entry.operands[slot] = view.operands[slot];
        }
        entry.symbol = view.symbol;
        entry.constant_le = view.constant_le;
        entry.constant_size = view.constant_size;
        snapshot.terms.push_back(entry);
    }
    for (std::size_t index = 0u; index < evidence.count; ++index) {
        snapshot.records.push_back(evidence.records[index]);
    }
    return snapshot;
}

ReportPtr replay(const ql_egraph_check_input_v1 &input) {
    ql_egraph_check_report *raw = nullptr;
    ql_error error{};
    EXPECT_EQ(QL_STATUS_OK,
              ql_egraph_check_replay(nullptr, &input, &raw, &error))
        << error.message;
    return ReportPtr(raw);
}

ql_egraph_check_report_view_v1 view_of(
    const ql_egraph_check_report *report) {
    ql_egraph_check_report_view_v1 view{};
    ql_error error{};
    EXPECT_EQ(QL_STATUS_OK,
              ql_egraph_check_report_get_view(report, &view, &error))
        << error.message;
    return view;
}

std::string findings_text(const ql_egraph_check_report_view_v1 &view) {
    std::string text;
    for (std::size_t index = 0u; index < view.finding_count; ++index) {
        const ql_egraph_check_finding_v1 &finding = view.findings[index];
        text += "\n  #";
        text += std::to_string(finding.sequence);
        text += " ";
        text += ql_egraph_check_verdict_string(finding.verdict);
        text += " ";
        text += ql_egraph_check_code_string(finding.code);
        text += " [";
        text += finding.rule_name;
        text += "] ";
        text += finding.detail;
    }
    return text;
}

TEST(EGraphReplayChecker, JustifiesEveryMergeAWideSaturationProduces) {
    const EGraphPtr graph = wide_sample();
    ql_egraph_check_report *raw = nullptr;
    ql_error error{};
    ASSERT_EQ(QL_STATUS_OK,
              ql_egraph_check_graph(nullptr, graph.get(), &raw, &error))
        << error.message;
    const ReportPtr report(raw);
    const ql_egraph_check_report_view_v1 view = view_of(report.get());

    EXPECT_GT(view.record_count, 20u)
        << "the sample stopped exercising the engine";
    EXPECT_EQ(0u, view.rejected_count) << findings_text(view);
    EXPECT_EQ(0u, view.assumed_count);
    EXPECT_EQ(view.record_count, view.justified_count);
    EXPECT_EQ(1u, view.all_merges_justified);
    EXPECT_EQ(0u, view.finding_count);
}

TEST(EGraphReplayChecker, AnAxiomIsAssumedAndNeverJustified) {
    const EGraphPtr graph = make_graph();
    ql_egraph *raw_graph = graph.get();
    const ql_egraph_term_id x = bv_variable(raw_graph, "x");
    const ql_egraph_term_id y = bv_variable(raw_graph, "y");
    ql_error error{};
    ASSERT_EQ(QL_STATUS_OK,
              ql_egraph_assume_equal(raw_graph, x, y, "caller.trusts.this",
                                     &error))
        << error.message;
    saturate(raw_graph);

    ql_egraph_check_report *raw = nullptr;
    ASSERT_EQ(QL_STATUS_OK,
              ql_egraph_check_graph(nullptr, raw_graph, &raw, &error))
        << error.message;
    const ReportPtr report(raw);
    const ql_egraph_check_report_view_v1 view = view_of(report.get());

    EXPECT_EQ(0u, view.rejected_count) << findings_text(view);
    EXPECT_EQ(1u, view.assumed_count);
    /* An assumption-relative log is consistent but not fully justified, and
       the flag must not blur the two. */
    EXPECT_EQ(0u, view.all_merges_justified);
    ASSERT_EQ(1u, view.finding_count);
    EXPECT_EQ(QL_EGRAPH_CHECK_VERDICT_ASSUMED, view.findings[0].verdict);
    EXPECT_EQ(QL_EGRAPH_CHECK_CODE_AXIOM, view.findings[0].code);
    EXPECT_STREQ("caller.trusts.this", view.findings[0].rule_name);

    std::uint32_t equal = 0u;
    ASSERT_EQ(QL_STATUS_OK,
              ql_egraph_check_terms_equal(report.get(), x, y, &equal,
                                          &error));
    EXPECT_EQ(1u, equal);
}

TEST(EGraphReplayChecker, TermEqualityComesFromTheReplayNotTheEngine) {
    const EGraphPtr graph = make_graph();
    ql_egraph *raw_graph = graph.get();
    const ql_egraph_term_id x = bv_variable(raw_graph, "x");
    const ql_egraph_term_id zero = bv_constant(raw_graph, 0u);
    const ql_egraph_term_id sum =
        operation(raw_graph, QL_EGRAPH_OP_BV_ADD, {x, zero});
    const ql_egraph_term_id y = bv_variable(raw_graph, "y");
    saturate(raw_graph);

    ql_egraph_check_report *raw = nullptr;
    ql_error error{};
    ASSERT_EQ(QL_STATUS_OK,
              ql_egraph_check_graph(nullptr, raw_graph, &raw, &error))
        << error.message;
    const ReportPtr report(raw);
    EXPECT_EQ(0u, view_of(report.get()).rejected_count)
        << findings_text(view_of(report.get()));

    std::uint32_t equal = 0u;
    ASSERT_EQ(QL_STATUS_OK,
              ql_egraph_check_terms_equal(report.get(), sum, x, &equal,
                                          &error));
    EXPECT_EQ(1u, equal);
    ASSERT_EQ(QL_STATUS_OK,
              ql_egraph_check_terms_equal(report.get(), sum, y, &equal,
                                          &error));
    EXPECT_EQ(0u, equal);
    EXPECT_EQ(QL_STATUS_INVALID_ARGUMENT,
              ql_egraph_check_terms_equal(report.get(), sum, 9999u, &equal,
                                          &error));
}

TEST(EGraphReplayChecker, RejectsARewriteWhoseSideConditionIsNotThere) {
    /* bvadd(x, y) is merged with a claim that y is the additive identity.
       Nothing in the graph makes y zero, so the rule cannot fire. */
    const EGraphPtr graph = make_graph();
    ql_egraph *raw_graph = graph.get();
    const ql_egraph_term_id x = bv_variable(raw_graph, "x");
    const ql_egraph_term_id y = bv_variable(raw_graph, "y");
    const ql_egraph_term_id sum =
        operation(raw_graph, QL_EGRAPH_OP_BV_ADD, {x, y});
    Snapshot snapshot = harvest(raw_graph);

    ql_egraph_merge_record_v1 forged{};
    forged.sequence = 1u;
    forged.kind = QL_EGRAPH_MERGE_REWRITE;
    forged.lhs_term = sum;
    forged.rhs_term = x;
    forged.lhs_class = snapshot.terms[sum - 1u].initial_class;
    forged.rhs_class = snapshot.terms[x - 1u].initial_class;
    forged.lhs_operand_count = 2u;
    forged.lhs_operands[0] = snapshot.terms[x - 1u].initial_class;
    forged.lhs_operands[1] = snapshot.terms[y - 1u].initial_class;
    forged.rhs_operand_count = 0u;
    std::strcpy(forged.reason, "bv.add.zero");
    snapshot.records.push_back(forged);

    const ReportPtr report = replay(snapshot.input());
    const ql_egraph_check_report_view_v1 view = view_of(report.get());
    ASSERT_EQ(1u, view.rejected_count) << findings_text(view);
    EXPECT_EQ(QL_EGRAPH_CHECK_CODE_SIDE_CONDITION, view.findings[0].code);
    EXPECT_EQ(0u, view.all_merges_justified);
}

TEST(EGraphReplayChecker, RejectsARewriteOnAnOperatorTheRuleDoesNotAdmit) {
    const EGraphPtr graph = make_graph();
    ql_egraph *raw_graph = graph.get();
    const ql_egraph_term_id x = bv_variable(raw_graph, "x");
    const ql_egraph_term_id zero = bv_constant(raw_graph, 0u);
    const ql_egraph_term_id product =
        operation(raw_graph, QL_EGRAPH_OP_BV_MUL, {x, zero});
    Snapshot snapshot = harvest(raw_graph);

    ql_egraph_merge_record_v1 forged{};
    forged.sequence = 1u;
    forged.kind = QL_EGRAPH_MERGE_REWRITE;
    forged.lhs_term = product;
    forged.rhs_term = zero;
    forged.lhs_class = snapshot.terms[product - 1u].initial_class;
    forged.rhs_class = snapshot.terms[zero - 1u].initial_class;
    forged.lhs_operand_count = 2u;
    forged.lhs_operands[0] = snapshot.terms[x - 1u].initial_class;
    forged.lhs_operands[1] = snapshot.terms[zero - 1u].initial_class;
    forged.rhs_operand_count = 0u;
    /* The conclusion happens to be true, but the rule named is the one for
       bitwise and, and a checker that waved it through would be trusting the
       name instead of the premise. */
    std::strcpy(forged.reason, "bv.and.zero");
    snapshot.records.push_back(forged);

    const ReportPtr report = replay(snapshot.input());
    const ql_egraph_check_report_view_v1 view = view_of(report.get());
    ASSERT_EQ(1u, view.rejected_count) << findings_text(view);
    EXPECT_EQ(QL_EGRAPH_CHECK_CODE_WRONG_OPERATOR, view.findings[0].code);
}

TEST(EGraphReplayChecker, RejectsAnUnknownRuleName) {
    const EGraphPtr graph = wide_sample();
    Snapshot snapshot = harvest(graph.get());
    std::size_t target = snapshot.records.size();
    for (std::size_t index = 0u; index < snapshot.records.size(); ++index) {
        if (snapshot.records[index].kind == QL_EGRAPH_MERGE_REWRITE) {
            target = index;
            break;
        }
    }
    ASSERT_LT(target, snapshot.records.size());
    std::strcpy(snapshot.records[target].reason, "bv.add.associative");

    const ReportPtr report = replay(snapshot.input());
    const ql_egraph_check_report_view_v1 view = view_of(report.get());
    ASSERT_GE(view.rejected_count, 1u);
    EXPECT_EQ(QL_EGRAPH_CHECK_CODE_UNKNOWN_RULE, view.findings[0].code);
}

TEST(EGraphReplayChecker, RejectsAStaleClassSnapshot) {
    const EGraphPtr graph = wide_sample();
    Snapshot snapshot = harvest(graph.get());
    ASSERT_FALSE(snapshot.records.empty());
    snapshot.records[0].lhs_class += 1u;

    const ReportPtr report = replay(snapshot.input());
    const ql_egraph_check_report_view_v1 view = view_of(report.get());
    ASSERT_GE(view.rejected_count, 1u);
    EXPECT_EQ(1u, view.findings[0].sequence);
    EXPECT_EQ(QL_EGRAPH_CHECK_CODE_STALE_CLASS, view.findings[0].code);
}

TEST(EGraphReplayChecker, RejectsAResequencedLog) {
    const EGraphPtr graph = wide_sample();
    Snapshot snapshot = harvest(graph.get());
    ASSERT_GE(snapshot.records.size(), 2u);
    snapshot.records.erase(snapshot.records.begin());

    const ReportPtr report = replay(snapshot.input());
    const ql_egraph_check_report_view_v1 view = view_of(report.get());
    ASSERT_GE(view.rejected_count, 1u);
    EXPECT_EQ(QL_EGRAPH_CHECK_CODE_SEQUENCE_GAP, view.findings[0].code);
}

TEST(EGraphReplayChecker, RejectsACongruenceWithUnequalOperands) {
    const EGraphPtr graph = make_graph();
    ql_egraph *raw_graph = graph.get();
    const ql_egraph_term_id x = bv_variable(raw_graph, "x");
    const ql_egraph_term_id y = bv_variable(raw_graph, "y");
    const ql_egraph_term_id left =
        operation(raw_graph, QL_EGRAPH_OP_BV_NOT, {x});
    const ql_egraph_term_id right =
        operation(raw_graph, QL_EGRAPH_OP_BV_NOT, {y});
    Snapshot snapshot = harvest(raw_graph);

    ql_egraph_merge_record_v1 forged{};
    forged.sequence = 1u;
    forged.kind = QL_EGRAPH_MERGE_CONGRUENCE;
    forged.lhs_term = left;
    forged.rhs_term = right;
    forged.lhs_class = snapshot.terms[left - 1u].initial_class;
    forged.rhs_class = snapshot.terms[right - 1u].initial_class;
    forged.lhs_operand_count = 1u;
    forged.lhs_operands[0] = snapshot.terms[x - 1u].initial_class;
    forged.rhs_operand_count = 1u;
    forged.rhs_operands[0] = snapshot.terms[y - 1u].initial_class;
    std::strcpy(forged.reason, "congruence");
    snapshot.records.push_back(forged);

    const ReportPtr report = replay(snapshot.input());
    const ql_egraph_check_report_view_v1 view = view_of(report.get());
    ASSERT_EQ(1u, view.rejected_count) << findings_text(view);
    EXPECT_EQ(QL_EGRAPH_CHECK_CODE_CONGRUENCE_MISMATCH,
              view.findings[0].code);
}

TEST(EGraphReplayChecker, ReportsEveryRejectionNotOnlyTheFirst) {
    const EGraphPtr graph = make_graph();
    ql_egraph *raw_graph = graph.get();
    const ql_egraph_term_id x = bv_variable(raw_graph, "x");
    const ql_egraph_term_id y = bv_variable(raw_graph, "y");
    const ql_egraph_term_id z = bv_variable(raw_graph, "z");
    const ql_egraph_term_id first =
        operation(raw_graph, QL_EGRAPH_OP_BV_ADD, {x, y});
    const ql_egraph_term_id second =
        operation(raw_graph, QL_EGRAPH_OP_BV_MUL, {x, z});
    Snapshot snapshot = harvest(raw_graph);

    ql_egraph_merge_record_v1 forged{};
    forged.kind = QL_EGRAPH_MERGE_REWRITE;
    forged.sequence = 1u;
    forged.lhs_term = first;
    forged.rhs_term = x;
    forged.lhs_class = snapshot.terms[first - 1u].initial_class;
    forged.rhs_class = snapshot.terms[x - 1u].initial_class;
    forged.lhs_operand_count = 2u;
    forged.lhs_operands[0] = snapshot.terms[x - 1u].initial_class;
    forged.lhs_operands[1] = snapshot.terms[y - 1u].initial_class;
    std::strcpy(forged.reason, "bv.add.zero");
    snapshot.records.push_back(forged);

    forged.sequence = 2u;
    forged.lhs_term = second;
    forged.rhs_term = z;
    forged.lhs_class = snapshot.terms[second - 1u].initial_class;
    forged.rhs_class = snapshot.terms[z - 1u].initial_class;
    forged.lhs_operands[1] = snapshot.terms[z - 1u].initial_class;
    std::strcpy(forged.reason, "bv.mul.one");
    snapshot.records.push_back(forged);

    const ReportPtr report = replay(snapshot.input());
    const ql_egraph_check_report_view_v1 view = view_of(report.get());
    /* Stopping at the first defect would hide the second one behind it. */
    EXPECT_EQ(2u, view.rejected_count) << findings_text(view);
    ASSERT_EQ(2u, view.finding_count);
    EXPECT_EQ(1u, view.findings[0].sequence);
    EXPECT_EQ(2u, view.findings[1].sequence);
}

TEST(EGraphReplayChecker, RefusesALogFromADifferentRewriteCatalogue) {
    const EGraphPtr graph = wide_sample();
    const Snapshot snapshot = harvest(graph.get());
    ql_egraph_check_input_v1 input = snapshot.input();
    input.rule_catalogue_digest.bytes[0] ^= 0xFFu;

    ql_egraph_check_report *raw = nullptr;
    ql_error error{};
    EXPECT_EQ(QL_STATUS_SCHEMA_MISMATCH,
              ql_egraph_check_replay(nullptr, &input, &raw, &error));
    EXPECT_EQ(nullptr, raw);

    input = snapshot.input();
    input.rule_catalogue_version += 1u;
    EXPECT_EQ(QL_STATUS_SCHEMA_MISMATCH,
              ql_egraph_check_replay(nullptr, &input, &raw, &error));
}

TEST(EGraphReplayChecker, RefusesAMalformedTermTable) {
    const EGraphPtr graph = wide_sample();
    Snapshot snapshot = harvest(graph.get());
    ql_egraph_check_report *raw = nullptr;
    ql_error error{};

    Snapshot cyclic = snapshot;
    ASSERT_GE(cyclic.terms.size(), 3u);
    for (std::size_t index = 0u; index < cyclic.terms.size(); ++index) {
        if (cyclic.terms[index].operand_count != 0u) {
            cyclic.terms[index].operands[0] = cyclic.terms[index].term;
            break;
        }
    }
    ql_egraph_check_input_v1 input = cyclic.input();
    EXPECT_EQ(QL_STATUS_CYCLE,
              ql_egraph_check_replay(nullptr, &input, &raw, &error));

    Snapshot sparse = snapshot;
    sparse.terms[1].term = 7u;
    input = sparse.input();
    EXPECT_EQ(QL_STATUS_INVALID_ARGUMENT,
              ql_egraph_check_replay(nullptr, &input, &raw, &error));

    Snapshot bad_abi = snapshot;
    input = bad_abi.input();
    input.abi_version += 1u;
    EXPECT_EQ(QL_STATUS_ABI_MISMATCH,
              ql_egraph_check_replay(nullptr, &input, &raw, &error));

    input = bad_abi.input();
    input.schema_version += 1u;
    EXPECT_EQ(QL_STATUS_SCHEMA_MISMATCH,
              ql_egraph_check_replay(nullptr, &input, &raw, &error));

    input = bad_abi.input();
    input.struct_size = sizeof(input) - 1u;
    EXPECT_EQ(QL_STATUS_ABI_MISMATCH,
              ql_egraph_check_replay(nullptr, &input, &raw, &error));
}

TEST(EGraphReplayChecker, RejectsARecordThatMergesAnAlreadyMergedPair) {
    const EGraphPtr graph = wide_sample();
    Snapshot snapshot = harvest(graph.get());
    ASSERT_FALSE(snapshot.records.empty());
    snapshot.records.push_back(snapshot.records[0]);
    snapshot.records.back().sequence =
        static_cast<std::uint64_t>(snapshot.records.size());

    const ReportPtr report = replay(snapshot.input());
    const ql_egraph_check_report_view_v1 view = view_of(report.get());
    ASSERT_GE(view.rejected_count, 1u);
    const ql_egraph_check_finding_v1 &last =
        view.findings[view.finding_count - 1u];
    /* The duplicate names classes that the first application already merged,
       so it is caught as a stale snapshot before it is caught as a no-op;
       either code means the log does not replay. */
    EXPECT_TRUE(last.code == QL_EGRAPH_CHECK_CODE_ALREADY_MERGED ||
                last.code == QL_EGRAPH_CHECK_CODE_STALE_CLASS)
        << ql_egraph_check_code_string(last.code);
}

TEST(EGraphReplayChecker, AnEmptyLogIsVacuouslyJustified) {
    const EGraphPtr graph = make_graph();
    bv_variable(graph.get(), "x");
    ql_egraph_check_report *raw = nullptr;
    ql_error error{};
    ASSERT_EQ(QL_STATUS_OK,
              ql_egraph_check_graph(nullptr, graph.get(), &raw, &error))
        << error.message;
    const ReportPtr report(raw);
    const ql_egraph_check_report_view_v1 view = view_of(report.get());
    EXPECT_EQ(0u, view.record_count);
    EXPECT_EQ(1u, view.all_merges_justified);
}

TEST(EGraphReplayChecker, RejectsNullArguments) {
    ql_egraph_check_report *raw = nullptr;
    ql_error error{};
    EXPECT_EQ(QL_STATUS_INVALID_ARGUMENT,
              ql_egraph_check_replay(nullptr, nullptr, &raw, &error));
    EXPECT_EQ(QL_STATUS_INVALID_ARGUMENT,
              ql_egraph_check_graph(nullptr, nullptr, &raw, &error));
    ql_egraph_check_report_view_v1 view{};
    EXPECT_EQ(QL_STATUS_INVALID_ARGUMENT,
              ql_egraph_check_report_get_view(nullptr, &view, &error));
    ql_egraph_check_report_release(nullptr);
}

}  // namespace
