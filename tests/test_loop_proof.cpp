#include "quodlibet/proof_smt.h"

#include <cstdint>
#include <cstring>
#include <string>

#include <gtest/gtest.h>

#include "loop_proof.h"
#include "w2_fixtures.h"

namespace {

constexpr char kTrusted[] = "{\"unsat_promotion\":\"trusted-backend\"}";

constexpr char kUnsignedWhile[] = "unsigned f(unsigned x, unsigned n) {"
                                  "  while (n != 0u) { x += 3u; --n; }"
                                  "  return x;"
                                  "}";

constexpr char kUnsignedFor[] = "unsigned g(unsigned x, unsigned n) {"
                                "  for (; n != 0u; --n) { x += 3u; }"
                                "  return x;"
                                "}";

bool BackendAvailable() {
  return ql_bitwuzla_solver_descriptor()->capability.availability !=
         QL_SOLVER_UNAVAILABLE;
}

class LoopOutcomeRun {
public:
  LoopOutcomeRun() = default;
  LoopOutcomeRun(const LoopOutcomeRun &) = delete;
  LoopOutcomeRun &operator=(const LoopOutcomeRun &) = delete;

  ~LoopOutcomeRun() {
    ql_artifact_release(outcome_);
    if (instance_ != nullptr) {
      ql_smt_product_method()->destroy(instance_);
    }
  }

  ql_status Run(const w2::Pair &pair, const char *options_json,
                ql_error *error) {
    ql_run_context_v1 context{};
    ql_artifact *input = pair.artifact();
    ql_status status = ql_smt_product_method()->create(
        ql_default_host(), options_json, &instance_, error);
    if (status != QL_STATUS_OK) {
      return status;
    }
    status = ql_smt_product_method()->validate(instance_, &input, 1u, error);
    if (status != QL_STATUS_OK) {
      return status;
    }
    context.struct_size = sizeof(context);
    context.abi_version = QL_ABI_VERSION;
    context.host = ql_default_host();
    return ql_smt_product_method()->run(instance_, &context, &input, 1u,
                                        &outcome_, error);
  }

  ql_smt_product_outcome_view_v1 outcome_view() const {
    ql_smt_product_outcome_view_v1 view{};
    ql_error error{};
    view.struct_size = sizeof(view);
    EXPECT_EQ(QL_STATUS_OK,
              ql_smt_product_outcome_read(outcome_, &view, &error))
        << error.message;
    return view;
  }

  ql_loop_proof_stats_v1 loop_stats() const {
    ql_loop_proof_stats_v1 stats{};
    ql_error error{};
    stats.struct_size = sizeof(stats);
    EXPECT_EQ(QL_STATUS_OK,
              ql_smt_product_outcome_loop_stats(outcome_, &stats, &error))
        << error.message;
    return stats;
  }

private:
  void *instance_ = nullptr;
  ql_artifact *outcome_ = nullptr;
};

class BuiltLoopQuery {
public:
  BuiltLoopQuery() = default;
  BuiltLoopQuery(const BuiltLoopQuery &) = delete;
  BuiltLoopQuery &operator=(const BuiltLoopQuery &) = delete;
  ~BuiltLoopQuery() { ql_loop_proof_query_destroy(query_); }

  ql_status Build(const w2::Pair &pair, ql_error *error) {
    return Build(pair.left_ir(), pair.right_ir(), error);
  }

  ql_status Build(const ql_ir *left, const ql_ir *right, ql_error *error) {
    ql_loop_proof_options_v1 options{};
    ql_loop_proof_options_init(&options);
    options.precondition_is_true = 1u;
    options.contract_binding_match = 1u;
    return ql_loop_proof_query_build(nullptr, left, right, &options, &query_,
                                     error);
  }

  ql_loop_proof_query *get() const { return query_; }

  ql_loop_proof_query_view_v1 view() const {
    ql_loop_proof_query_view_v1 result{};
    ql_error error{};
    result.struct_size = sizeof(result);
    EXPECT_EQ(QL_STATUS_OK,
              ql_loop_proof_query_get_view(query_, &result, &error))
        << error.message;
    return result;
  }

  ql_loop_relation_candidate_v1 candidate(std::size_t index) const {
    ql_loop_relation_candidate_v1 result{};
    ql_error error{};
    result.struct_size = sizeof(result);
    EXPECT_EQ(QL_STATUS_OK,
              ql_loop_proof_query_candidate_at(query_, index, &result, &error))
        << error.message;
    return result;
  }

private:
  ql_loop_proof_query *query_ = nullptr;
};

class RawAdditiveLoop {
public:
  RawAdditiveLoop() = default;
  RawAdditiveLoop(const RawAdditiveLoop &) = delete;
  RawAdditiveLoop &operator=(const RawAdditiveLoop &) = delete;
  ~RawAdditiveLoop() {
    ql_ir_release(ir_);
    ql_artifact_release(artifact_);
    ql_ir_builder_destroy(builder_);
  }

  ql_status Build(ql_error *error) {
    ql_ir_type_definition_v1 type{};
    ql_ir_type_id bool_type = QL_IR_INVALID_TYPE_ID;
    ql_ir_type_id bv8 = QL_IR_INVALID_TYPE_ID;
    ql_ir_block_id entry = QL_IR_INVALID_BLOCK_ID;
    ql_ir_block_id header = QL_IR_INVALID_BLOCK_ID;
    ql_ir_block_id exit = QL_IR_INVALID_BLOCK_ID;
    ql_ir_value_id zero = QL_IR_INVALID_VALUE_ID;
    ql_ir_value_id one = QL_IR_INVALID_VALUE_ID;
    ql_ir_value_id count = QL_IR_INVALID_VALUE_ID;
    ql_ir_value_id next_count = QL_IR_INVALID_VALUE_ID;
    ql_ir_value_id condition = QL_IR_INVALID_VALUE_ID;
    ql_ir_instruction_id phi_id = QL_IR_INVALID_INSTRUCTION_ID;
    ql_ir_instruction_id instruction_id = QL_IR_INVALID_INSTRUCTION_ID;
    const std::uint8_t zero_byte = 0u;
    const std::uint8_t one_byte = 1u;
    ql_status status = ql_ir_builder_create(nullptr, &builder_, error);

    if (status == QL_STATUS_OK) {
      ql_ir_type_definition_init(&type, QL_IR_TYPE_BOOL);
      type.bit_width = 1u;
      status = ql_ir_builder_add_type(builder_, &type, &bool_type, error);
    }
    if (status == QL_STATUS_OK) {
      ql_ir_type_definition_init(&type, QL_IR_TYPE_BIT_VECTOR);
      type.bit_width = 8u;
      status = ql_ir_builder_add_type(builder_, &type, &bv8, error);
    }
    if (status == QL_STATUS_OK) {
      status = ql_ir_builder_set_function(builder_, "raw_add", 7u, bv8, error);
    }
    if (status == QL_STATUS_OK) {
      status = ql_ir_builder_set_cfg_kind(builder_, QL_IR_CFG_CYCLIC, error);
    }
    if (status == QL_STATUS_OK) {
      status = ql_ir_builder_add_constant(builder_, bv8, &zero_byte, 1u, &zero,
                                          error);
    }
    if (status == QL_STATUS_OK) {
      status =
          ql_ir_builder_add_constant(builder_, bv8, &one_byte, 1u, &one, error);
    }
    if (status == QL_STATUS_OK) {
      status = ql_ir_builder_add_block(builder_, "entry", 5u, &entry, error);
    }
    if (status == QL_STATUS_OK) {
      status = ql_ir_builder_add_block(builder_, "header", 6u, &header, error);
    }
    if (status == QL_STATUS_OK) {
      status = ql_ir_builder_add_block(builder_, "exit", 4u, &exit, error);
    }
    if (status == QL_STATUS_OK) {
      status = ql_ir_builder_set_entry_block(builder_, entry, error);
    }
    if (status == QL_STATUS_OK) {
      ql_ir_terminator_definition_v1 branch{};
      ql_ir_terminator_definition_init(&branch, QL_IR_TERMINATOR_BRANCH);
      branch.target = header;
      status = ql_ir_builder_set_terminator(builder_, entry, &branch, error);
    }
    if (status == QL_STATUS_OK) {
      const ql_ir_type_id result_type = bv8;
      ql_ir_instruction_definition_v1 phi{};
      ql_ir_instruction_definition_init(&phi, QL_IR_OPCODE_PHI);
      phi.operands = &zero;
      phi.operand_count = 1u;
      phi.block_operands = &entry;
      phi.block_operand_count = 1u;
      phi.result_types = &result_type;
      phi.result_count = 1u;
      status = ql_ir_builder_append_instruction(builder_, header, &phi, &phi_id,
                                                &count, error);
    }
    if (status == QL_STATUS_OK) {
      const ql_ir_value_id operands[2] = {count, one};
      const ql_ir_type_id result_type = bv8;
      ql_ir_instruction_definition_v1 add{};
      ql_ir_instruction_definition_init(&add, QL_IR_OPCODE_ADD);
      add.operands = operands;
      add.operand_count = 2u;
      add.result_types = &result_type;
      add.result_count = 1u;
      status = ql_ir_builder_append_instruction(
          builder_, header, &add, &instruction_id, &next_count, error);
    }
    if (status == QL_STATUS_OK) {
      const ql_ir_value_id operands[2] = {count, one};
      const ql_ir_type_id result_type = bool_type;
      ql_ir_instruction_definition_v1 compare{};
      ql_ir_instruction_definition_init(&compare, QL_IR_OPCODE_ULT);
      compare.operands = operands;
      compare.operand_count = 2u;
      compare.result_types = &result_type;
      compare.result_count = 1u;
      status = ql_ir_builder_append_instruction(
          builder_, header, &compare, &instruction_id, &condition, error);
    }
    if (status == QL_STATUS_OK) {
      status = ql_ir_builder_append_phi_incoming(builder_, phi_id, next_count,
                                                 header, error);
    }
    if (status == QL_STATUS_OK) {
      ql_ir_terminator_definition_v1 branch{};
      ql_ir_terminator_definition_init(&branch, QL_IR_TERMINATOR_COND_BRANCH);
      branch.condition = condition;
      branch.target = header;
      branch.false_target = exit;
      status = ql_ir_builder_set_terminator(builder_, header, &branch, error);
    }
    if (status == QL_STATUS_OK) {
      ql_ir_terminator_definition_v1 result{};
      ql_ir_terminator_definition_init(&result, QL_IR_TERMINATOR_RETURN);
      result.return_value = count;
      status = ql_ir_builder_set_terminator(builder_, exit, &result, error);
    }
    if (status == QL_STATUS_OK) {
      status = ql_ir_builder_finish(builder_, &artifact_, error);
    }
    if (status == QL_STATUS_OK) {
      status = ql_ir_open(nullptr, artifact_, &ir_, error);
    }
    return status;
  }

  ql_ir *get() const { return ir_; }

private:
  ql_ir_builder *builder_ = nullptr;
  ql_artifact *artifact_ = nullptr;
  ql_ir *ir_ = nullptr;
};

std::string ArtifactText(const ql_artifact *artifact) {
  ql_artifact_view view{};
  ql_error error{};
  view.struct_size = sizeof(view);
  EXPECT_NE(nullptr, artifact);
  if (artifact == nullptr ||
      ql_artifact_get_view(artifact, &view, &error) != QL_STATUS_OK) {
    ADD_FAILURE() << error.message;
    return {};
  }
  return {static_cast<const char *>(view.data), view.size};
}

void ExpectExactSharedSelfPair(const char *source, const char *function,
                               std::uint64_t expected_loop_count) {
  w2::Pair pair;
  BuiltLoopQuery query;
  LoopOutcomeRun run;
  ql_error error{};

  ASSERT_EQ(QL_STATUS_OK,
            pair.Build(source, function, source, function,
                       w2::ContractObserving(QL_OBSERVE_RETURN_VALUE), &error))
      << error.message;
  ASSERT_EQ(QL_STATUS_OK, query.Build(pair, &error)) << error.message;
  const ql_loop_proof_query_view_v1 query_view = query.view();
  ASSERT_EQ(QL_LOOP_PROOF_QUERY_READY, query_view.disposition)
      << query_view.diagnostic;
  EXPECT_EQ(1u, query_view.self_pair);
  EXPECT_EQ(1u, query_view.ir_structural_match);
  EXPECT_EQ(1u, query_view.structural_query_available);
  EXPECT_EQ(1u, query_view.promotion_eligible);
  EXPECT_NE(nullptr, std::strstr(query_view.diagnostic, "reflexivity"));
  EXPECT_EQ(nullptr, ql_loop_proof_query_summary_artifact(query.get()));
  const std::string prefix =
      ArtifactText(ql_loop_proof_query_prefix_artifact(query.get()));
  EXPECT_NE(std::string::npos, prefix.find("(declare-fun ql_lp_step_"));
  EXPECT_EQ(std::string::npos, prefix.find("unroll"));
  EXPECT_EQ(std::string::npos, prefix.find("bounded"));

  ASSERT_EQ(QL_STATUS_OK, run.Run(pair, kTrusted, &error)) << error.message;
  const ql_smt_product_outcome_view_v1 outcome = run.outcome_view();
  const ql_loop_proof_stats_v1 loop = run.loop_stats();
  EXPECT_EQ(QL_VERDICT_PROVED_EQUIVALENT, outcome.verdict);
  EXPECT_EQ(QL_EVIDENCE_PROOF, outcome.evidence_class);
  EXPECT_EQ(QL_SMT_PRODUCT_ANSWER_UNSAT, outcome.violation_answer);
  EXPECT_EQ(QL_SMT_PRODUCT_ANSWER_SAT, outcome.domain_answer);
  EXPECT_EQ(0u, outcome.replay_confirmed);
  EXPECT_EQ(1u, loop.proof_eligible);
  EXPECT_EQ(expected_loop_count, loop.left_loop_count);
  EXPECT_EQ(expected_loop_count, loop.right_loop_count);
  EXPECT_EQ(expected_loop_count, loop.paired_loop_count);
  EXPECT_EQ(expected_loop_count, loop.induction_proved_count);
  EXPECT_EQ(QL_SMT_PRODUCT_ANSWER_UNSAT, loop.induction_answer);
  EXPECT_EQ(QL_SMT_PRODUCT_ANSWER_NOT_QUERIED, loop.summary_answer);
  EXPECT_EQ(0u, loop.summary_attempted_count);
  EXPECT_EQ(0u, loop.summary_proved_count);
  EXPECT_EQ(0u, loop.fallback_reached);
  EXPECT_EQ(QL_LOOP_STAGE_DISCOVER | QL_LOOP_STAGE_CANONICALIZE |
                QL_LOOP_STAGE_PAIRING | QL_LOOP_STAGE_INVARIANT |
                QL_LOOP_STAGE_INDUCTION,
            loop.stage_reached);
  ql_digest zero{};
  EXPECT_EQ(0u, ql_digest_equal(&loop.base_obligation_digest, &zero));
  EXPECT_EQ(0u, ql_digest_equal(&loop.guard_obligation_digest, &zero));
  EXPECT_EQ(0u, ql_digest_equal(&loop.step_obligation_digest, &zero));
  EXPECT_EQ(0u, ql_digest_equal(&loop.exit_obligation_digest, &zero));
}

void ExpectExactReflexivitySelfPair(const char *source, const char *function,
                                    std::uint64_t expected_loop_count,
                                    const ql_semantic_contract_v1 &contract) {
  w2::Pair pair;
  BuiltLoopQuery query;
  LoopOutcomeRun trusted_run;
  LoopOutcomeRun untrusted_run;
  ql_error error{};

  ASSERT_EQ(QL_STATUS_OK,
            pair.Build(source, function, source, function, contract, &error))
      << error.message;
  ASSERT_EQ(QL_STATUS_OK, query.Build(pair, &error)) << error.message;
  const ql_loop_proof_query_view_v1 query_view = query.view();
  ASSERT_EQ(QL_LOOP_PROOF_QUERY_READY, query_view.disposition)
      << query_view.diagnostic;
  EXPECT_EQ(QL_LOOP_PROOF_STRATEGY_EXACT_REFLEXIVITY, query_view.strategy);
  EXPECT_EQ(1u, query_view.self_pair);
  EXPECT_EQ(1u, query_view.ir_structural_match);
  EXPECT_EQ(1u, query_view.all_loops_paired);
  EXPECT_EQ(1u, query_view.metrics.concrete_domain_witness);
  EXPECT_EQ(0u, query_view.metrics.invariant_generated_count);
  EXPECT_EQ(nullptr, ql_loop_proof_query_induction_artifact(query.get()));
  EXPECT_NE(nullptr, ql_loop_proof_query_reflexivity_artifact(query.get()));
  const std::string prefix =
      ArtifactText(ql_loop_proof_query_prefix_artifact(query.get()));
  EXPECT_NE(std::string::npos,
            prefix.find("quodlibet_exact_reflexivity_bad"));
  EXPECT_EQ(std::string::npos, prefix.find("unroll"));
  EXPECT_EQ(std::string::npos, prefix.find("bounded"));

  ASSERT_EQ(QL_STATUS_OK, trusted_run.Run(pair, kTrusted, &error))
      << error.message;
  const ql_smt_product_outcome_view_v1 outcome = trusted_run.outcome_view();
  const ql_loop_proof_stats_v1 loop = trusted_run.loop_stats();
  EXPECT_EQ(QL_VERDICT_PROVED_EQUIVALENT, outcome.verdict);
  EXPECT_EQ(QL_EVIDENCE_PROOF, outcome.evidence_class);
  EXPECT_EQ(QL_SMT_PRODUCT_ANSWER_UNSAT, outcome.violation_answer);
  EXPECT_EQ(QL_SMT_PRODUCT_ANSWER_SAT, outcome.domain_answer);
  EXPECT_EQ(0u, outcome.checked_proof);
  EXPECT_EQ(0u, outcome.replay_confirmed);
  EXPECT_EQ(QL_LOOP_PROOF_STRATEGY_EXACT_REFLEXIVITY, loop.strategy);
  EXPECT_EQ(expected_loop_count, loop.left_loop_count);
  EXPECT_EQ(expected_loop_count, loop.right_loop_count);
  EXPECT_EQ(expected_loop_count, loop.paired_loop_count);
  EXPECT_EQ(expected_loop_count, loop.reflexivity_proved_count);
  EXPECT_EQ(0u, loop.invariant_generated_count);
  EXPECT_EQ(0u, loop.induction_proved_count);
  EXPECT_EQ(QL_SMT_PRODUCT_ANSWER_NOT_QUERIED, loop.induction_answer);
  EXPECT_EQ(QL_SMT_PRODUCT_ANSWER_UNSAT, loop.reflexivity_answer);
  EXPECT_EQ(1u, loop.concrete_domain_witness);
  EXPECT_NE(0u, loop.stage_reached & QL_LOOP_STAGE_REFLEXIVITY);
  EXPECT_EQ(0u, loop.fallback_reached);
  ql_digest zero{};
  EXPECT_EQ(0u, ql_digest_equal(&loop.reflexivity_obligation_digest, &zero));
  EXPECT_EQ(0u, ql_digest_equal(&loop.domain_witness_digest, &zero));

  ASSERT_EQ(QL_STATUS_OK, untrusted_run.Run(pair, nullptr, &error))
      << error.message;
  const ql_smt_product_outcome_view_v1 untrusted =
      untrusted_run.outcome_view();
  EXPECT_EQ(QL_VERDICT_UNKNOWN, untrusted.verdict);
  EXPECT_EQ(QL_SMT_PRODUCT_ANSWER_UNSAT, untrusted.violation_answer);
  EXPECT_EQ(QL_SMT_PRODUCT_ANSWER_SAT, untrusted.domain_answer);
  EXPECT_NE(nullptr, std::strstr(untrusted.diagnostic, "evidence only"));
}

TEST(LoopProof, UnsignedScalarSelfPairUsesStructuralInduction) {
  w2::Pair pair;
  LoopOutcomeRun run;
  ql_error error{};

  if (!BackendAvailable()) {
    GTEST_SKIP() << "Bitwuzla support is disabled";
  }
  ASSERT_EQ(QL_STATUS_OK,
            pair.Build(kUnsignedWhile, "f", kUnsignedWhile, "f",
                       w2::ContractObserving(QL_OBSERVE_RETURN_VALUE), &error))
      << error.message;
  ASSERT_EQ(QL_STATUS_OK, run.Run(pair, kTrusted, &error)) << error.message;

  const ql_smt_product_outcome_view_v1 outcome = run.outcome_view();
  const ql_loop_proof_stats_v1 loop = run.loop_stats();
  EXPECT_EQ(QL_VERDICT_PROVED_EQUIVALENT, outcome.verdict);
  EXPECT_EQ(QL_EVIDENCE_PROOF, outcome.evidence_class);
  EXPECT_EQ(0u, outcome.checked_proof);
  EXPECT_EQ(QL_SMT_PRODUCT_ANSWER_UNSAT, outcome.violation_answer);
  EXPECT_EQ(QL_SMT_PRODUCT_ANSWER_SAT, outcome.domain_answer);

  EXPECT_EQ(QL_LOOP_PROOF_STATS_SCHEMA_VERSION, loop.schema_version);
  EXPECT_EQ(1u, loop.applicable);
  EXPECT_EQ(1u, loop.cyclic);
  EXPECT_EQ(1u, loop.left_loop_count);
  EXPECT_EQ(1u, loop.right_loop_count);
  EXPECT_EQ(1u, loop.natural_loop_count);
  EXPECT_EQ(1u, loop.paired_loop_count);
  EXPECT_EQ(1u, loop.all_loops_paired);
  EXPECT_GT(loop.invariant_generated_count, 0u);
  EXPECT_EQ(1u, loop.induction_proved_count);
  EXPECT_EQ(1u, loop.proof_eligible);
  EXPECT_EQ(QL_LOOP_PROOF_STRATEGY_STRUCTURAL_INDUCTION, loop.strategy);
  EXPECT_EQ(QL_SMT_PRODUCT_ANSWER_UNSAT, loop.induction_answer);
  EXPECT_NE(0u, loop.stage_reached & QL_LOOP_STAGE_DISCOVER);
  EXPECT_NE(0u, loop.stage_reached & QL_LOOP_STAGE_CANONICALIZE);
  EXPECT_NE(0u, loop.stage_reached & QL_LOOP_STAGE_PAIRING);
  EXPECT_NE(0u, loop.stage_reached & QL_LOOP_STAGE_INVARIANT);
  EXPECT_NE(0u, loop.stage_reached & QL_LOOP_STAGE_INDUCTION);

  ql_digest zero{};
  EXPECT_EQ(0u, ql_digest_equal(&loop.canonical_digest, &zero));
  EXPECT_EQ(0u, ql_digest_equal(&loop.base_obligation_digest, &zero));
  EXPECT_EQ(0u, ql_digest_equal(&loop.guard_obligation_digest, &zero));
  EXPECT_EQ(0u, ql_digest_equal(&loop.step_obligation_digest, &zero));
  EXPECT_EQ(0u, ql_digest_equal(&loop.exit_obligation_digest, &zero));
}

TEST(LoopProof, RawLoopInductionUnsatIsOnlyEvidenceWithoutTrust) {
  w2::Pair pair;
  LoopOutcomeRun run;
  ql_error error{};

  if (!BackendAvailable()) {
    GTEST_SKIP() << "Bitwuzla support is disabled";
  }
  ASSERT_EQ(QL_STATUS_OK,
            pair.Build(kUnsignedWhile, "f", kUnsignedWhile, "f",
                       w2::ContractObserving(QL_OBSERVE_RETURN_VALUE), &error))
      << error.message;
  ASSERT_EQ(QL_STATUS_OK, run.Run(pair, nullptr, &error)) << error.message;

  const ql_smt_product_outcome_view_v1 outcome = run.outcome_view();
  const ql_loop_proof_stats_v1 loop = run.loop_stats();
  EXPECT_EQ(QL_VERDICT_UNKNOWN, outcome.verdict);
  EXPECT_EQ(QL_EVIDENCE_UNKNOWN, outcome.evidence_class);
  EXPECT_EQ(QL_UNSAT_PROMOTION_NONE, outcome.unsat_promotion);
  EXPECT_EQ(QL_SMT_PRODUCT_ANSWER_UNSAT, outcome.violation_answer);
  EXPECT_EQ(QL_SMT_PRODUCT_ANSWER_SAT, outcome.domain_answer);
  EXPECT_EQ(QL_LOOP_PROOF_STRATEGY_STRUCTURAL_INDUCTION, loop.strategy);
  EXPECT_EQ(QL_SMT_PRODUCT_ANSWER_UNSAT, loop.induction_answer);
  EXPECT_EQ(1u, loop.induction_proved_count);
  EXPECT_NE(nullptr, std::strstr(outcome.diagnostic, "evidence only"));
}

TEST(LoopProof, ProvesSameLoopBodyAcrossDifferentFunctionNames) {
  constexpr char right[] = "unsigned g(unsigned x, unsigned n) {"
                           "  while (n != 0u) { x += 3u; --n; }"
                           "  return x;"
                           "}";
  w2::Pair pair;
  BuiltLoopQuery query;
  LoopOutcomeRun run;
  ql_error error{};

  if (!BackendAvailable()) {
    GTEST_SKIP() << "Bitwuzla support is disabled";
  }
  ASSERT_EQ(QL_STATUS_OK,
            pair.Build(kUnsignedWhile, "f", right, "g",
                       w2::ContractObserving(QL_OBSERVE_RETURN_VALUE), &error))
      << error.message;
  ASSERT_EQ(QL_STATUS_OK, query.Build(pair, &error)) << error.message;
  EXPECT_EQ(std::string::npos,
            ArtifactText(ql_loop_proof_query_prefix_artifact(query.get()))
                .find("unroll"));
  ASSERT_EQ(QL_STATUS_OK, run.Run(pair, kTrusted, &error)) << error.message;

  const ql_smt_product_outcome_view_v1 outcome = run.outcome_view();
  const ql_loop_proof_stats_v1 loop = run.loop_stats();
  EXPECT_EQ(QL_VERDICT_PROVED_EQUIVALENT, outcome.verdict);
  EXPECT_EQ(QL_SMT_PRODUCT_ANSWER_UNSAT, outcome.violation_answer);
  EXPECT_EQ(QL_SMT_PRODUCT_ANSWER_SAT, outcome.domain_answer);
  EXPECT_EQ(QL_LOOP_PROOF_STRATEGY_STRUCTURAL_INDUCTION, loop.strategy);
  EXPECT_EQ(1u, loop.all_loops_paired);
  EXPECT_EQ(1u, loop.induction_proved_count);
  EXPECT_EQ(1u, loop.proof_eligible);
  EXPECT_EQ(0u, loop.fallback_reached);
  EXPECT_EQ(0u, outcome.replay_confirmed);
  ql_digest zero{};
  EXPECT_EQ(0u, ql_digest_equal(&loop.base_obligation_digest, &zero));
  EXPECT_EQ(0u, ql_digest_equal(&loop.guard_obligation_digest, &zero));
  EXPECT_EQ(0u, ql_digest_equal(&loop.step_obligation_digest, &zero));
  EXPECT_EQ(0u, ql_digest_equal(&loop.exit_obligation_digest, &zero));
}

TEST(LoopProof, ProvesForStepAgainstEquivalentWhileBody) {
  w2::Pair pair;
  BuiltLoopQuery query;
  LoopOutcomeRun run;
  ql_error error{};

  if (!BackendAvailable()) {
    GTEST_SKIP() << "Bitwuzla support is disabled";
  }
  ASSERT_EQ(QL_STATUS_OK,
            pair.Build(kUnsignedWhile, "f", kUnsignedFor, "g",
                       w2::ContractObserving(QL_OBSERVE_RETURN_VALUE), &error))
      << error.message;
  ASSERT_EQ(QL_STATUS_OK, query.Build(pair, &error)) << error.message;
  EXPECT_EQ(std::string::npos,
            ArtifactText(ql_loop_proof_query_prefix_artifact(query.get()))
                .find("unroll"));
  ASSERT_EQ(QL_STATUS_OK, run.Run(pair, kTrusted, &error)) << error.message;

  const ql_smt_product_outcome_view_v1 outcome = run.outcome_view();
  const ql_loop_proof_stats_v1 loop = run.loop_stats();
  EXPECT_EQ(QL_VERDICT_PROVED_EQUIVALENT, outcome.verdict);
  EXPECT_EQ(0u, outcome.replay_confirmed);
  EXPECT_EQ(1u, loop.all_loops_paired);
  EXPECT_EQ(1u, loop.paired_loop_count);
  EXPECT_EQ(0u, loop.fallback_reached);
  EXPECT_EQ(0u, loop.fallback_attempted);
  EXPECT_EQ(QL_LOOP_PROOF_STRATEGY_STRUCTURAL_INDUCTION, loop.strategy);
  EXPECT_EQ(QL_SMT_PRODUCT_ANSWER_UNSAT, loop.induction_answer);
  EXPECT_EQ(1u, loop.induction_proved_count);
  ql_digest zero{};
  EXPECT_EQ(0u, ql_digest_equal(&loop.base_obligation_digest, &zero));
  EXPECT_EQ(0u, ql_digest_equal(&loop.guard_obligation_digest, &zero));
  EXPECT_EQ(0u, ql_digest_equal(&loop.step_obligation_digest, &zero));
  EXPECT_EQ(0u, ql_digest_equal(&loop.exit_obligation_digest, &zero));
}

TEST(LoopProof, GuardMismatchFallsBackWithoutInventingACounterexample) {
  constexpr char other[] = "unsigned g(unsigned x, unsigned n) {"
                           "  while (n > 1u) { x += 3u; --n; }"
                           "  return x;"
                           "}";
  w2::Pair pair;
  LoopOutcomeRun run;
  ql_error error{};

  if (!BackendAvailable()) {
    GTEST_SKIP() << "Bitwuzla support is disabled";
  }
  ASSERT_EQ(QL_STATUS_OK,
            pair.Build(kUnsignedWhile, "f", other, "g",
                       w2::ContractObserving(QL_OBSERVE_RETURN_VALUE), &error))
      << error.message;
  ASSERT_EQ(QL_STATUS_OK, run.Run(pair, kTrusted, &error)) << error.message;

  const ql_smt_product_outcome_view_v1 outcome = run.outcome_view();
  const ql_loop_proof_stats_v1 loop = run.loop_stats();
  EXPECT_EQ(QL_VERDICT_UNKNOWN, outcome.verdict);
  EXPECT_NE(QL_VERDICT_COUNTEREXAMPLE, outcome.verdict);
  EXPECT_EQ(0u, outcome.replay_confirmed);
  EXPECT_EQ(1u, loop.fallback_reached);
  EXPECT_EQ(0u, loop.fallback_attempted);
  EXPECT_NE(0u, loop.stage_reached & QL_LOOP_STAGE_FALLBACK);
  /* A candidate-level SAT can reject an invariant or summary, but it is not
     a whole-program witness and therefore must never populate a replay. */
  EXPECT_EQ(nullptr, std::strstr(outcome.diagnostic, "concrete replay"));
}

TEST(LoopProof, StepMismatchNeverReusesTheSharedTransitionFunction) {
  constexpr char other[] = "unsigned g(unsigned x, unsigned n) {"
                           "  while (n != 0u) { x += 4u; --n; }"
                           "  return x;"
                           "}";
  w2::Pair pair;
  LoopOutcomeRun run;
  ql_error error{};

  ASSERT_EQ(QL_STATUS_OK,
            pair.Build(kUnsignedWhile, "f", other, "g",
                       w2::ContractObserving(QL_OBSERVE_RETURN_VALUE), &error))
      << error.message;
  ASSERT_EQ(QL_STATUS_OK, run.Run(pair, kTrusted, &error)) << error.message;

  const ql_smt_product_outcome_view_v1 outcome = run.outcome_view();
  const ql_loop_proof_stats_v1 loop = run.loop_stats();
  EXPECT_EQ(QL_VERDICT_UNKNOWN, outcome.verdict);
  EXPECT_EQ(0u, outcome.replay_confirmed);
  EXPECT_EQ(1u, loop.all_loops_paired);
  EXPECT_EQ(1u, loop.paired_loop_count);
  EXPECT_EQ(0u, loop.invariant_generated_count);
  EXPECT_EQ(QL_SMT_PRODUCT_ANSWER_NOT_QUERIED, loop.induction_answer);
  EXPECT_EQ(1u, loop.fallback_reached);
  EXPECT_EQ(QL_LOOP_PROOF_STRATEGY_CHC_PDR_UNAVAILABLE, loop.strategy);
  EXPECT_NE(nullptr, std::strstr(loop.failure_reason, "relation"));
}

TEST(LoopProof, LoopCountMismatchStaysUnknownAtTheFallbackBoundary) {
  constexpr char two_loops[] = "unsigned g(unsigned x, unsigned n) {"
                               "  while (n != 0u) { ++x; --n; }"
                               "  while (x != 0u) { --x; }"
                               "  return x;"
                               "}";
  w2::Pair pair;
  LoopOutcomeRun run;
  ql_error error{};

  if (!BackendAvailable()) {
    GTEST_SKIP() << "Bitwuzla support is disabled";
  }
  ASSERT_EQ(QL_STATUS_OK,
            pair.Build(kUnsignedWhile, "f", two_loops, "g",
                       w2::ContractObserving(QL_OBSERVE_RETURN_VALUE), &error))
      << error.message;
  ASSERT_EQ(QL_STATUS_OK, run.Run(pair, kTrusted, &error)) << error.message;

  const ql_smt_product_outcome_view_v1 outcome = run.outcome_view();
  const ql_loop_proof_stats_v1 loop = run.loop_stats();
  EXPECT_EQ(QL_VERDICT_UNKNOWN, outcome.verdict);
  EXPECT_NE(QL_VERDICT_COUNTEREXAMPLE, outcome.verdict);
  EXPECT_EQ(0u, outcome.replay_confirmed);
  EXPECT_EQ(1u, loop.left_loop_count);
  EXPECT_EQ(2u, loop.right_loop_count);
  EXPECT_EQ(0u, loop.all_loops_paired);
  EXPECT_EQ(1u, loop.fallback_reached);
  EXPECT_EQ(0u, loop.fallback_attempted);
  EXPECT_EQ(QL_LOOP_PROOF_STRATEGY_CHC_PDR_UNAVAILABLE, loop.strategy);
}

TEST(LoopProof, OneSidedLoopFallbackReportsOnlyReachedStages) {
  constexpr char straight[] =
      "unsigned g(unsigned x, unsigned n) { return x + 3u * n; }";
  w2::Pair pair;
  LoopOutcomeRun run;
  ql_error error{};

  ASSERT_EQ(QL_STATUS_OK,
            pair.Build(kUnsignedWhile, "f", straight, "g",
                       w2::ContractObserving(QL_OBSERVE_RETURN_VALUE), &error))
      << error.message;
  ASSERT_EQ(QL_STATUS_OK, run.Run(pair, kTrusted, &error)) << error.message;

  const ql_smt_product_outcome_view_v1 outcome = run.outcome_view();
  const ql_loop_proof_stats_v1 loop = run.loop_stats();
  EXPECT_EQ(QL_VERDICT_UNKNOWN, outcome.verdict);
  EXPECT_EQ(1u, loop.applicable);
  EXPECT_EQ(1u, loop.left_loop_count);
  EXPECT_EQ(0u, loop.right_loop_count);
  EXPECT_EQ(1u, loop.fallback_reached);
  EXPECT_EQ(QL_LOOP_PROOF_STRATEGY_CHC_PDR_UNAVAILABLE, loop.strategy);
  EXPECT_EQ(QL_LOOP_STAGE_DISCOVER | QL_LOOP_STAGE_FALLBACK,
            loop.stage_reached);
  EXPECT_EQ(QL_SMT_PRODUCT_ANSWER_NOT_QUERIED, loop.induction_answer);
  EXPECT_EQ(QL_SMT_PRODUCT_ANSWER_NOT_QUERIED, loop.summary_answer);
}

TEST(LoopProof, MemoryLoopIsExplicitlyUnsupportedAndStaysUnknown) {
  constexpr char memory_left[] = "unsigned f(unsigned *p, unsigned n) {"
                                 "  while (n != 0u) { *p += 1u; ++p; --n; }"
                                 "  return n;"
                                 "}";
  constexpr char memory_right[] =
      "unsigned g(unsigned *p, unsigned n) {"
      "  for (; n != 0u; --n, ++p) { *p = *p + 1u; }"
      "  return n;"
      "}";
  w2::Pair pair;
  LoopOutcomeRun run;
  ql_error error{};

  if (!BackendAvailable()) {
    GTEST_SKIP() << "Bitwuzla support is disabled";
  }
  ASSERT_EQ(QL_STATUS_OK, pair.Build(memory_left, "f", memory_right, "g",
                                     w2::DefaultContract(), &error))
      << error.message;
  ASSERT_EQ(QL_STATUS_OK, run.Run(pair, kTrusted, &error)) << error.message;

  const ql_smt_product_outcome_view_v1 outcome = run.outcome_view();
  const ql_loop_proof_stats_v1 loop = run.loop_stats();
  EXPECT_EQ(QL_VERDICT_UNKNOWN, outcome.verdict);
  EXPECT_NE(QL_VERDICT_COUNTEREXAMPLE, outcome.verdict);
  EXPECT_EQ(1u, loop.applicable);
  EXPECT_EQ(1u, loop.fallback_reached);
  EXPECT_EQ(0u, loop.fallback_attempted);
  EXPECT_EQ(0u, loop.proof_eligible);
  EXPECT_NE('\0', loop.failure_reason[0]);
}

TEST(LoopProof, QueryContainsBaseGuardStepAndExitWithoutUnrolling) {
  w2::Pair pair;
  BuiltLoopQuery query;
  ql_error error{};

  ASSERT_EQ(QL_STATUS_OK,
            pair.Build(kUnsignedWhile, "f", kUnsignedWhile, "f",
                       w2::ContractObserving(QL_OBSERVE_RETURN_VALUE), &error))
      << error.message;
  ASSERT_EQ(QL_STATUS_OK, query.Build(pair, &error)) << error.message;

  const ql_loop_proof_query_view_v1 view = query.view();
  ASSERT_EQ(QL_LOOP_PROOF_QUERY_READY, view.disposition);
  ASSERT_EQ(1u, view.structural_query_available);
  ASSERT_NE(nullptr, ql_loop_proof_query_induction_artifact(query.get()));
  const std::string prefix =
      ArtifactText(ql_loop_proof_query_prefix_artifact(query.get()));
  EXPECT_NE(std::string::npos, prefix.find("quodlibet_loop_base_bad"));
  EXPECT_NE(std::string::npos, prefix.find("quodlibet_loop_guard_bad"));
  EXPECT_NE(std::string::npos, prefix.find("quodlibet_loop_step_bad"));
  EXPECT_NE(std::string::npos, prefix.find("quodlibet_loop_exit_bad"));
  EXPECT_NE(std::string::npos, prefix.find("quodlibet_loop_induction_bad"));
  EXPECT_NE(std::string::npos, prefix.find("(define-fun ql_lp_l_next_"));
  EXPECT_NE(std::string::npos, prefix.find("(define-fun ql_lp_r_next_"));
  EXPECT_NE(std::string::npos, prefix.find("bvadd"));
  EXPECT_EQ(std::string::npos, prefix.find("(declare-fun ql_lp_step_"));
  EXPECT_EQ(std::string::npos, prefix.find("unroll"));
  EXPECT_EQ(std::string::npos, prefix.find("bounded"));
}

TEST(LoopProof, DisconnectedSummaryIsNotExposedAsAProofCandidate) {
  RawAdditiveLoop ir;
  BuiltLoopQuery query;
  ql_error error{};

  ASSERT_EQ(QL_STATUS_OK, ir.Build(&error)) << error.message;
  ASSERT_EQ(QL_STATUS_OK, query.Build(ir.get(), ir.get(), &error))
      << error.message;
  const ql_loop_proof_query_view_v1 initial = query.view();
  ASSERT_EQ(QL_LOOP_PROOF_QUERY_READY, initial.disposition)
      << initial.diagnostic;
  EXPECT_EQ(1u, initial.metrics.summary_candidate_count);
  EXPECT_EQ(0u, initial.affine_summary_available);
  EXPECT_EQ(nullptr, ql_loop_proof_query_summary_artifact(query.get()));

  ASSERT_EQ(QL_STATUS_OK,
            ql_loop_proof_query_record_check(
                query.get(), QL_LOOP_PROOF_CHECK_INDUCTION, QL_SOLVER_CHECK_SAT,
                QL_SOLVER_UNKNOWN_NONE, 17u, &error))
      << error.message;
  ql_loop_proof_query_view_v1 view = query.view();
  EXPECT_EQ(QL_SMT_PRODUCT_ANSWER_SAT, view.metrics.induction_answer);
  EXPECT_EQ(QL_SMT_PRODUCT_ANSWER_NOT_QUERIED, view.metrics.summary_answer);
  EXPECT_EQ(0u, view.metrics.summary_attempted_count);
  EXPECT_EQ(QL_LOOP_PROOF_STRATEGY_CHC_PDR_UNAVAILABLE, view.strategy);
  EXPECT_EQ(1u, view.chc_pdr_reached);
  EXPECT_EQ(0u, view.candidate_sat_is_counterexample);
}

TEST(LoopProof, ProvesASelectedConstantOffsetInvariant) {
  constexpr char offset_left[] = "unsigned f(unsigned n) {"
                                 "  unsigned x = 3u;"
                                 "  while (n != 0u) { ++x; --n; }"
                                 "  return x;"
                                 "}";
  constexpr char offset_right[] = "unsigned g(unsigned n) {"
                                  "  unsigned y = 0u;"
                                  "  while (n != 0u) { ++y; --n; }"
                                  "  return y + 3u;"
                                  "}";
  w2::Pair pair;
  BuiltLoopQuery query;
  LoopOutcomeRun run;
  ql_error error{};

  if (!BackendAvailable()) {
    GTEST_SKIP() << "Bitwuzla support is disabled";
  }
  ASSERT_EQ(QL_STATUS_OK,
            pair.Build(offset_left, "f", offset_right, "g",
                       w2::ContractObserving(QL_OBSERVE_RETURN_VALUE), &error))
      << error.message;
  ASSERT_EQ(QL_STATUS_OK, query.Build(pair, &error)) << error.message;
  const ql_loop_proof_query_view_v1 view = query.view();
  EXPECT_EQ(QL_LOOP_PROOF_QUERY_READY, view.disposition) << view.diagnostic;
  EXPECT_EQ(1u, view.all_loops_paired);
  bool found_offset = false;
  for (std::size_t index = 0u; index < view.candidate_count; ++index) {
    const ql_loop_relation_candidate_v1 candidate = query.candidate(index);
    if (candidate.kind == QL_LOOP_RELATION_CONSTANT_OFFSET &&
        candidate.multiplier == 1u && candidate.offset == 3u) {
      EXPECT_EQ(1u, candidate.selected);
      found_offset = true;
    }
  }
  EXPECT_TRUE(found_offset);
  EXPECT_EQ(std::string::npos,
            ArtifactText(ql_loop_proof_query_prefix_artifact(query.get()))
                .find("unroll"));

  ASSERT_EQ(QL_STATUS_OK, run.Run(pair, kTrusted, &error)) << error.message;
  const ql_smt_product_outcome_view_v1 outcome = run.outcome_view();
  const ql_loop_proof_stats_v1 loop = run.loop_stats();
  EXPECT_EQ(QL_VERDICT_PROVED_EQUIVALENT, outcome.verdict);
  EXPECT_EQ(QL_SMT_PRODUCT_ANSWER_UNSAT, loop.induction_answer);
  EXPECT_EQ(0u, outcome.replay_confirmed);
  ql_digest zero{};
  EXPECT_EQ(0u, ql_digest_equal(&loop.base_obligation_digest, &zero));
  EXPECT_EQ(0u, ql_digest_equal(&loop.guard_obligation_digest, &zero));
  EXPECT_EQ(0u, ql_digest_equal(&loop.step_obligation_digest, &zero));
  EXPECT_EQ(0u, ql_digest_equal(&loop.exit_obligation_digest, &zero));
}

TEST(LoopProof, ProvesASelectedAffineInvariant) {
  constexpr char affine_left[] = "unsigned f(unsigned n) {"
                                 "  unsigned x = 0u;"
                                 "  while (n != 0u) { x += 2u; --n; }"
                                 "  return x;"
                                 "}";
  constexpr char affine_right[] = "unsigned g(unsigned n) {"
                                  "  unsigned y = 0u;"
                                  "  while (n != 0u) { ++y; --n; }"
                                  "  return y * 2u;"
                                  "}";
  w2::Pair pair;
  BuiltLoopQuery query;
  LoopOutcomeRun run;
  ql_error error{};

  if (!BackendAvailable()) {
    GTEST_SKIP() << "Bitwuzla support is disabled";
  }
  ASSERT_EQ(QL_STATUS_OK,
            pair.Build(affine_left, "f", affine_right, "g",
                       w2::ContractObserving(QL_OBSERVE_RETURN_VALUE), &error))
      << error.message;
  ASSERT_EQ(QL_STATUS_OK, query.Build(pair, &error)) << error.message;
  const ql_loop_proof_query_view_v1 view = query.view();
  EXPECT_EQ(QL_LOOP_PROOF_QUERY_READY, view.disposition) << view.diagnostic;
  EXPECT_EQ(1u, view.all_loops_paired);
  bool found_affine = false;
  for (std::size_t index = 0u; index < view.candidate_count; ++index) {
    const ql_loop_relation_candidate_v1 candidate = query.candidate(index);
    if (candidate.kind == QL_LOOP_RELATION_AFFINE &&
        candidate.multiplier == 2u && candidate.offset == 0u) {
      EXPECT_EQ(1u, candidate.selected);
      found_affine = true;
    }
  }
  EXPECT_TRUE(found_affine);
  EXPECT_EQ(std::string::npos,
            ArtifactText(ql_loop_proof_query_prefix_artifact(query.get()))
                .find("unroll"));

  ASSERT_EQ(QL_STATUS_OK, run.Run(pair, kTrusted, &error)) << error.message;
  const ql_smt_product_outcome_view_v1 outcome = run.outcome_view();
  const ql_loop_proof_stats_v1 loop = run.loop_stats();
  EXPECT_EQ(QL_VERDICT_PROVED_EQUIVALENT, outcome.verdict);
  EXPECT_EQ(QL_SMT_PRODUCT_ANSWER_UNSAT, loop.induction_answer);
  EXPECT_EQ(0u, outcome.replay_confirmed);
  ql_digest zero{};
  EXPECT_EQ(0u, ql_digest_equal(&loop.base_obligation_digest, &zero));
  EXPECT_EQ(0u, ql_digest_equal(&loop.guard_obligation_digest, &zero));
  EXPECT_EQ(0u, ql_digest_equal(&loop.step_obligation_digest, &zero));
  EXPECT_EQ(0u, ql_digest_equal(&loop.exit_obligation_digest, &zero));
}

TEST(LoopProof, ExactDoWhileSelfPairUsesSharedStructuralInduction) {
  constexpr char source[] = "unsigned f(unsigned x, unsigned n) {"
                            "  do { x += 3u; n -= 1u; } while (n != 0u);"
                            "  return x;"
                            "}";

  if (!BackendAvailable()) {
    GTEST_SKIP() << "Bitwuzla support is disabled";
  }
  ExpectExactSharedSelfPair(source, "f", 1u);
}

TEST(LoopProof, ExactSequentialTwoLoopSelfPairUsesSharedStructuralInduction) {
  constexpr char source[] = "unsigned f(unsigned x, unsigned n, unsigned m) {"
                            "  while (n != 0u) { ++x; --n; }"
                            "  while (m != 0u) { x += 2u; --m; }"
                            "  return x;"
                            "}";

  if (!BackendAvailable()) {
    GTEST_SKIP() << "Bitwuzla support is disabled";
  }
  ExpectExactSharedSelfPair(source, "f", 2u);
}

TEST(LoopProof, ExactNonAffineSelfPairFallsBackToSharedCongruence) {
  constexpr char source[] = "unsigned f(unsigned x, unsigned n) {"
                            "  while (n != 0u) { x ^= n; --n; }"
                            "  return x;"
                            "}";

  if (!BackendAvailable()) {
    GTEST_SKIP() << "Bitwuzla support is disabled";
  }
  ExpectExactSharedSelfPair(source, "f", 1u);
}

TEST(LoopProof, ExactEffectfulSelfPairUsesConcreteDomainReflexivity) {
  constexpr char source[] = "unsigned total;"
                            "unsigned f(unsigned n) {"
                            "  while (n != 0u) { total += n; --n; }"
                            "  return total;"
                            "}";

  if (!BackendAvailable()) {
    GTEST_SKIP() << "Bitwuzla support is disabled";
  }
  ExpectExactReflexivitySelfPair(
      source, "f", 1u,
      w2::ContractObserving(QL_OBSERVE_RETURN_VALUE | QL_OBSERVE_MEMORY));
}

TEST(LoopProof, ExactUbSelfPairFindsANonzeroDefinedDomainWitness) {
  constexpr char source[] = "int f(int x) {"
                            "  while (x == 0) { x = 1 / x; }"
                            "  return x;"
                            "}";

  if (!BackendAvailable()) {
    GTEST_SKIP() << "Bitwuzla support is disabled";
  }
  ExpectExactReflexivitySelfPair(
      source, "f", 1u, w2::ContractObserving(QL_OBSERVE_RETURN_VALUE));
}

TEST(LoopProof, ExactUbSelfPairWithoutDefinedWitnessStaysUnknown) {
  constexpr char source[] = "int f(int x) {"
                            "  while (x == x) { x = 1 / (x - x); }"
                            "  return x;"
                            "}";
  w2::Pair pair;
  BuiltLoopQuery query;
  LoopOutcomeRun run;
  ql_error error{};

  if (!BackendAvailable()) {
    GTEST_SKIP() << "Bitwuzla support is disabled";
  }
  ASSERT_EQ(QL_STATUS_OK,
            pair.Build(source, "f", source, "f",
                       w2::ContractObserving(QL_OBSERVE_RETURN_VALUE), &error))
      << error.message;
  ASSERT_EQ(QL_STATUS_OK, query.Build(pair, &error)) << error.message;
  EXPECT_EQ(0u, query.view().metrics.concrete_domain_witness);
  EXPECT_EQ(0u, query.view().promotion_eligible);

  ASSERT_EQ(QL_STATUS_OK, run.Run(pair, kTrusted, &error)) << error.message;
  const ql_smt_product_outcome_view_v1 outcome = run.outcome_view();
  const ql_loop_proof_stats_v1 loop = run.loop_stats();
  EXPECT_EQ(QL_VERDICT_UNKNOWN, outcome.verdict);
  EXPECT_EQ(0u, loop.proof_eligible);
  EXPECT_EQ(0u, loop.concrete_domain_witness);
  EXPECT_EQ(QL_SMT_PRODUCT_ANSWER_NOT_QUERIED, loop.reflexivity_answer);
  EXPECT_NE(nullptr,
            std::strstr(loop.failure_reason, "undefined-behavior"));
}

TEST(LoopProof, ExactAmbiguousGuardSelfPairUsesConcreteDomainReflexivity) {
  constexpr char source[] = "int f(int x) {"
                            "  for (;;) {"
                            "    ++x;"
                            "    if (x == 10) break;"
                            "  }"
                            "  return x;"
                            "}";

  if (!BackendAvailable()) {
    GTEST_SKIP() << "Bitwuzla support is disabled";
  }
  ExpectExactReflexivitySelfPair(
      source, "f", 1u, w2::ContractObserving(QL_OBSERVE_RETURN_VALUE));
}

TEST(LoopProof, ExactNestedSelfPairUsesConcreteDomainReflexivity) {
  constexpr char source[] = "int f(int n) {"
                            "  int sum = 0;"
                            "  for (int i = 0; i < n; ++i) {"
                            "    for (int j = 0; j < n; ++j) sum += i + j;"
                            "  }"
                            "  return sum;"
                            "}";

  if (!BackendAvailable()) {
    GTEST_SKIP() << "Bitwuzla support is disabled";
  }
  ExpectExactReflexivitySelfPair(
      source, "f", 2u, w2::ContractObserving(QL_OBSERVE_RETURN_VALUE));
}

TEST(LoopProof, SyntacticLoopWithoutABackedgeDelegatesToAcyclicProduct) {
  constexpr char loop[] =
      "unsigned f(unsigned x) { while (x) { break; } return x; }";
  constexpr char straight[] = "unsigned g(unsigned x) { return x; }";
  w2::Pair pair;
  LoopOutcomeRun run;
  ql_error error{};

  if (!BackendAvailable()) {
    GTEST_SKIP() << "Bitwuzla support is disabled";
  }
  ASSERT_EQ(QL_STATUS_OK,
            pair.Build(loop, "f", straight, "g",
                       w2::ContractObserving(QL_OBSERVE_RETURN_VALUE), &error))
      << error.message;
  ASSERT_EQ(QL_STATUS_OK, run.Run(pair, kTrusted, &error)) << error.message;

  EXPECT_EQ(QL_VERDICT_PROVED_EQUIVALENT, run.outcome_view().verdict);
  const ql_loop_proof_stats_v1 stats = run.loop_stats();
  EXPECT_EQ(0u, stats.applicable);
  EXPECT_EQ(0u, stats.natural_loop_count);
  EXPECT_EQ(QL_LOOP_PROOF_STRATEGY_NONE, stats.strategy);
}

} // namespace
