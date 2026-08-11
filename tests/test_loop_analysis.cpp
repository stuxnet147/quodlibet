#include "loop_analysis.h"

#include "quodlibet/c_lower.h"

#include <cstddef>
#include <cstdlib>
#include <cstring>
#include <initializer_list>

#include <gtest/gtest.h>

namespace {

class AnalyzedFunction {
public:
  ~AnalyzedFunction() {
    ql_loop_analysis_destroy(analysis_);
    ql_ir_release(ir_);
    ql_c_lower_result_destroy(result_);
    ql_c_frontend_unit_destroy(unit_);
  }

  bool Open(const char *source, const char *name) {
    ql_c_function_view function{};
    ql_c_lower_result_view_v1 lowered{};
    ql_error error{};
    const std::size_t source_size = std::strlen(source);

    if (ql_c_frontend_analyze(nullptr, source, source_size, &unit_, &error) !=
        QL_STATUS_OK) {
      ADD_FAILURE() << "analyze: " << error.message;
      return false;
    }
    function.struct_size = sizeof(function);
    if (ql_c_frontend_select_function(unit_, name, std::strlen(name), &function,
                                      &error) != QL_STATUS_OK) {
      ADD_FAILURE() << "select: " << error.message;
      return false;
    }
    if (ql_c_lower_selected_function(nullptr, source, source_size, unit_,
                                     &function, &result_, &error) !=
        QL_STATUS_OK) {
      ADD_FAILURE() << "lower: " << error.message;
      return false;
    }
    lowered.struct_size = sizeof(lowered);
    if (ql_c_lower_result_get_view(result_, &lowered, &error) !=
        QL_STATUS_OK) {
      ADD_FAILURE() << "lower view: " << error.message;
      return false;
    }
    if (lowered.support != QL_C_LOWER_SUPPORTED) {
      ADD_FAILURE() << "source is outside the C lowering profile";
      return false;
    }
    if (ql_ir_open(nullptr, lowered.ir_artifact, &ir_, &error) !=
        QL_STATUS_OK) {
      ADD_FAILURE() << "IR open: " << error.message;
      return false;
    }
    if (ql_loop_analysis_create(nullptr, ir_, &analysis_, &error) !=
        QL_STATUS_OK) {
      ADD_FAILURE() << "loop analysis: " << error.message;
      return false;
    }
    return true;
  }

  const ql_loop_analysis_view *view() const {
    return ql_loop_analysis_get_view(analysis_);
  }

  const ql_loop_view *loop(std::size_t index) const {
    return ql_loop_analysis_loop_at(analysis_, index);
  }

  const ql_ir *ir() const { return ir_; }

  bool Contains(std::size_t loop_index, ql_ir_block_id block) const {
    return ql_loop_analysis_contains_block(analysis_, loop_index, block) != 0u;
  }

private:
  ql_c_frontend_unit *unit_ = nullptr;
  ql_c_lower_result *result_ = nullptr;
  ql_ir *ir_ = nullptr;
  ql_loop_analysis *analysis_ = nullptr;
};

void ExpectCanonicalSingleLoop(const AnalyzedFunction &function,
                               ql_loop_guard_position guard_position) {
  const ql_loop_analysis_view *analysis = function.view();
  ASSERT_NE(nullptr, analysis);
  ASSERT_EQ(1u, analysis->loop_count);
  EXPECT_EQ(1u, analysis->declared_cyclic);
  EXPECT_EQ(1u, analysis->has_actual_cycle);
  EXPECT_EQ(0u, analysis->has_unclassified_cycle);
  EXPECT_GE(analysis->natural_backedge_count, 1u);

  const ql_loop_view *loop = function.loop(0u);
  ASSERT_NE(nullptr, loop);
  EXPECT_EQ(0u, loop->index);
  EXPECT_EQ(QL_LOOP_INVALID_INDEX, loop->parent);
  EXPECT_EQ(0u, loop->nesting_depth);
  EXPECT_EQ(1u, loop->is_reducible_single_entry);
  EXPECT_NE(QL_IR_INVALID_BLOCK_ID, loop->preheader);
  EXPECT_GE(loop->block_count, 2u);
  EXPECT_GE(loop->latch_count, 1u);
  EXPECT_GE(loop->exit_count, 1u);
  EXPECT_EQ(guard_position, loop->guard.position);
  EXPECT_TRUE(function.Contains(0u, loop->header));
  EXPECT_FALSE(function.Contains(0u, loop->preheader));
  for (std::size_t index = 0u; index < loop->latch_count; ++index) {
    EXPECT_TRUE(function.Contains(0u, loop->latches[index]));
  }
}

TEST(LoopAnalysis, CanonicalizesForWhileAndDoWhileByCfgRole) {
  constexpr char for_source[] =
      "int loop_for(int n) {\n"
      "  int i = 0;\n"
      "  int sum = 0;\n"
      "  for (; i < n; ++i) sum += 3;\n"
      "  return sum;\n"
      "}\n";
  constexpr char while_source[] =
      "int loop_while(int n) {\n"
      "  int i = 0;\n"
      "  int sum = 0;\n"
      "  while (i < n) { sum += 3; ++i; }\n"
      "  return sum;\n"
      "}\n";
  constexpr char do_source[] =
      "int loop_do(int n) {\n"
      "  int i = 0;\n"
      "  int sum = 0;\n"
      "  do { sum += 3; ++i; } while (i < n);\n"
      "  return sum;\n"
      "}\n";
  AnalyzedFunction for_loop;
  AnalyzedFunction while_loop;
  AnalyzedFunction do_loop;

  ASSERT_TRUE(for_loop.Open(for_source, "loop_for"));
  ASSERT_TRUE(while_loop.Open(while_source, "loop_while"));
  ASSERT_TRUE(do_loop.Open(do_source, "loop_do"));
  ExpectCanonicalSingleLoop(for_loop, QL_LOOP_GUARD_PRE_TEST);
  ExpectCanonicalSingleLoop(while_loop, QL_LOOP_GUARD_PRE_TEST);
  ExpectCanonicalSingleLoop(do_loop, QL_LOOP_GUARD_POST_TEST);

  const ql_loop_view *for_view = for_loop.loop(0u);
  const ql_loop_view *while_view = while_loop.loop(0u);
  const ql_loop_view *do_view = do_loop.loop(0u);
  ASSERT_NE(nullptr, for_view);
  ASSERT_NE(nullptr, while_view);
  ASSERT_NE(nullptr, do_view);
  EXPECT_EQ(for_view->phi_count, while_view->phi_count);
  EXPECT_EQ(for_view->phi_count, do_view->phi_count);
}

TEST(LoopAnalysis,
     ForUpdateAndWhileBodyShareDescriptorIgnoringBlockPartition) {
  constexpr char while_source[] =
      "unsigned loop_while(unsigned x, unsigned n) {\n"
      "  while (n != 0u) { x += 3u; --n; }\n"
      "  return x;\n"
      "}\n";
  constexpr char for_source[] =
      "unsigned loop_for(unsigned x, unsigned n) {\n"
      "  for (; n != 0u; --n) { x += 3u; }\n"
      "  return x;\n"
      "}\n";
  AnalyzedFunction while_function;
  AnalyzedFunction for_function;
  ASSERT_TRUE(while_function.Open(while_source, "loop_while"));
  ASSERT_TRUE(for_function.Open(for_source, "loop_for"));
  ASSERT_EQ(1u, while_function.view()->loop_count);
  ASSERT_EQ(1u, for_function.view()->loop_count);
  const ql_loop_view *while_loop = while_function.loop(0u);
  const ql_loop_view *for_loop = for_function.loop(0u);
  ASSERT_NE(nullptr, while_loop);
  ASSERT_NE(nullptr, for_loop);

  /* The for update is an administrative block boundary. Pairing may ignore
     this count, but proof promotion still needs the separately serialized
     entry, guard, transition, and exit semantics. */
  EXPECT_NE(while_loop->block_count, for_loop->block_count);
  EXPECT_EQ(while_loop->latch_count, for_loop->latch_count);
  EXPECT_EQ(while_loop->exit_count, for_loop->exit_count);
  EXPECT_EQ(while_loop->phi_count, for_loop->phi_count);
  EXPECT_EQ(while_loop->guard.position, for_loop->guard.position);
  EXPECT_EQ(while_loop->guard.continue_on_true,
            for_loop->guard.continue_on_true);
  EXPECT_EQ(while_loop->nesting_depth, for_loop->nesting_depth);
  EXPECT_EQ(while_loop->is_reducible_single_entry,
            for_loop->is_reducible_single_entry);
  ASSERT_EQ(QL_LOOP_GUARD_PRE_TEST, while_loop->guard.position);
  ASSERT_EQ(1u, while_loop->latch_count);
  ASSERT_EQ(1u, while_loop->exit_count);

  for (std::size_t index = 0u; index < while_loop->phi_count; ++index) {
    const ql_loop_phi_view &left = while_loop->phis[index];
    const ql_loop_phi_view &right = for_loop->phis[index];
    EXPECT_EQ(left.type_kind, right.type_kind) << "PHI " << index;
    EXPECT_EQ(left.bit_width, right.bit_width) << "PHI " << index;
    EXPECT_EQ(left.latch_value_count, right.latch_value_count)
        << "PHI " << index;
    EXPECT_EQ(left.recurrence, right.recurrence) << "PHI " << index;
    EXPECT_EQ(left.multiplier, right.multiplier) << "PHI " << index;
    EXPECT_EQ(left.offset, right.offset) << "PHI " << index;
  }
}

TEST(LoopAnalysis, BuildsNaturalLoopNestingTree) {
  constexpr char source[] =
      "int nested(int n) {\n"
      "  int sum = 0;\n"
      "  for (int i = 0; i < n; ++i) {\n"
      "    for (int j = 0; j < n; ++j) sum += i + j;\n"
      "  }\n"
      "  return sum;\n"
      "}\n";
  AnalyzedFunction function;
  ASSERT_TRUE(function.Open(source, "nested"));
  ASSERT_NE(nullptr, function.view());
  ASSERT_EQ(2u, function.view()->loop_count);

  std::size_t outer_index = QL_LOOP_INVALID_INDEX;
  std::size_t inner_index = QL_LOOP_INVALID_INDEX;
  for (std::size_t index = 0u; index < function.view()->loop_count; ++index) {
    const ql_loop_view *loop = function.loop(index);
    ASSERT_NE(nullptr, loop);
    ASSERT_EQ(1u, loop->is_reducible_single_entry);
    if (loop->nesting_depth == 0u) {
      ASSERT_EQ(QL_LOOP_INVALID_INDEX, outer_index);
      outer_index = index;
    } else if (loop->nesting_depth == 1u) {
      ASSERT_EQ(QL_LOOP_INVALID_INDEX, inner_index);
      inner_index = index;
    }
  }
  ASSERT_NE(QL_LOOP_INVALID_INDEX, outer_index);
  ASSERT_NE(QL_LOOP_INVALID_INDEX, inner_index);
  ASSERT_EQ(outer_index, function.loop(inner_index)->parent);
  EXPECT_TRUE(function.Contains(outer_index, function.loop(inner_index)->header));
  EXPECT_LT(function.loop(inner_index)->block_count,
            function.loop(outer_index)->block_count);
}

TEST(LoopAnalysis, DeclaredCyclicWithoutBackedgeHasNoNaturalLoop) {
  constexpr char source[] =
      "int break_only(int take) {\n"
      "  while (take) { break; }\n"
      "  return take;\n"
      "}\n";
  AnalyzedFunction function;
  ASSERT_TRUE(function.Open(source, "break_only"));
  const ql_loop_analysis_view *view = function.view();
  ASSERT_NE(nullptr, view);
  EXPECT_EQ(1u, view->declared_cyclic);
  EXPECT_EQ(0u, view->has_actual_cycle);
  EXPECT_EQ(0u, view->has_unclassified_cycle);
  EXPECT_EQ(0u, view->natural_backedge_count);
  EXPECT_EQ(0u, view->loop_count);
  EXPECT_EQ(nullptr, function.loop(0u));
}

TEST(LoopAnalysis, SeparatesIrreducibleCycleFromNaturalLoops) {
  ql_ir_builder *builder = nullptr;
  ql_artifact *artifact = nullptr;
  ql_ir *ir = nullptr;
  ql_loop_analysis *analysis = nullptr;
  ql_error error{};
  ASSERT_EQ(QL_STATUS_OK, ql_ir_builder_create(nullptr, &builder, &error));

  ql_ir_type_definition_v1 type{};
  ql_ir_type_id bool_type = QL_IR_INVALID_TYPE_ID;
  ql_ir_type_id bv8 = QL_IR_INVALID_TYPE_ID;
  ql_ir_type_definition_init(&type, QL_IR_TYPE_BOOL);
  type.bit_width = 1u;
  ASSERT_EQ(QL_STATUS_OK,
            ql_ir_builder_add_type(builder, &type, &bool_type, &error));
  ql_ir_type_definition_init(&type, QL_IR_TYPE_BIT_VECTOR);
  type.bit_width = 8u;
  ASSERT_EQ(QL_STATUS_OK,
            ql_ir_builder_add_type(builder, &type, &bv8, &error));
  ASSERT_EQ(QL_STATUS_OK,
            ql_ir_builder_set_function(builder, "irreducible", 11u, bv8,
                                       &error));
  ASSERT_EQ(QL_STATUS_OK,
            ql_ir_builder_set_cfg_kind(builder, QL_IR_CFG_CYCLIC, &error));

  const uint8_t true_byte = 1u;
  const uint8_t zero_byte = 0u;
  ql_ir_value_id condition = QL_IR_INVALID_VALUE_ID;
  ql_ir_value_id zero = QL_IR_INVALID_VALUE_ID;
  ASSERT_EQ(QL_STATUS_OK,
            ql_ir_builder_add_constant(builder, bool_type, &true_byte, 1u,
                                       &condition, &error));
  ASSERT_EQ(QL_STATUS_OK,
            ql_ir_builder_add_constant(builder, bv8, &zero_byte, 1u, &zero,
                                       &error));

  auto add_block = [&](const char *label) {
    ql_ir_block_id block = QL_IR_INVALID_BLOCK_ID;
    EXPECT_EQ(QL_STATUS_OK,
              ql_ir_builder_add_block(builder, label, std::strlen(label),
                                      &block, &error))
        << error.message;
    return block;
  };
  const ql_ir_block_id entry = add_block("entry");
  const ql_ir_block_id left = add_block("not_a_loop_name");
  const ql_ir_block_id right = add_block("also_not_a_loop_name");
  const ql_ir_block_id exit = add_block("exit");
  ASSERT_EQ(QL_STATUS_OK,
            ql_ir_builder_set_entry_block(builder, entry, &error));

  auto set_conditional = [&](ql_ir_block_id block, ql_ir_block_id on_true,
                             ql_ir_block_id on_false) {
    ql_ir_terminator_definition_v1 term{};
    ql_ir_terminator_definition_init(&term, QL_IR_TERMINATOR_COND_BRANCH);
    term.condition = condition;
    term.target = on_true;
    term.false_target = on_false;
    return ql_ir_builder_set_terminator(builder, block, &term, &error);
  };
  ASSERT_EQ(QL_STATUS_OK, set_conditional(entry, left, right));
  ASSERT_EQ(QL_STATUS_OK, set_conditional(left, right, exit));
  ql_ir_terminator_definition_v1 branch{};
  ql_ir_terminator_definition_init(&branch, QL_IR_TERMINATOR_BRANCH);
  branch.target = left;
  ASSERT_EQ(QL_STATUS_OK,
            ql_ir_builder_set_terminator(builder, right, &branch, &error));
  ql_ir_terminator_definition_v1 result{};
  ql_ir_terminator_definition_init(&result, QL_IR_TERMINATOR_RETURN);
  result.return_value = zero;
  ASSERT_EQ(QL_STATUS_OK,
            ql_ir_builder_set_terminator(builder, exit, &result, &error));

  ASSERT_EQ(QL_STATUS_OK, ql_ir_builder_finish(builder, &artifact, &error))
      << error.message;
  ASSERT_EQ(QL_STATUS_OK, ql_ir_open(nullptr, artifact, &ir, &error))
      << error.message;
  ASSERT_EQ(QL_STATUS_OK,
            ql_loop_analysis_create(nullptr, ir, &analysis, &error))
      << error.message;
  const ql_loop_analysis_view *view = ql_loop_analysis_get_view(analysis);
  ASSERT_NE(nullptr, view);
  EXPECT_EQ(1u, view->has_actual_cycle);
  EXPECT_EQ(1u, view->has_unclassified_cycle);
  EXPECT_EQ(0u, view->natural_backedge_count);
  EXPECT_EQ(0u, view->loop_count);

  ql_loop_analysis_destroy(analysis);
  ql_ir_release(ir);
  ql_artifact_release(artifact);
  ql_ir_builder_destroy(builder);
}

TEST(LoopAnalysis, ClassifiesScalarAffineAndPointerRecurrences) {
  constexpr char source[] =
      "int recurrences(int n, int *p) {\n"
      "  int i = 0;\n"
      "  int down = n;\n"
      "  int affine = 1;\n"
      "  while (i < n) {\n"
      "    ++i;\n"
      "    down -= 2;\n"
      "    affine = affine * 3 + 4;\n"
      "    ++p;\n"
      "  }\n"
      "  return i + down + affine + (p != 0);\n"
      "}\n";
  AnalyzedFunction function;
  ASSERT_TRUE(function.Open(source, "recurrences"));
  ASSERT_EQ(1u, function.view()->loop_count);
  const ql_loop_view *loop = function.loop(0u);
  ASSERT_NE(nullptr, loop);

  bool saw_add_one = false;
  bool saw_sub_two = false;
  bool saw_affine = false;
  bool saw_pointer_stride = false;
  for (std::size_t index = 0u; index < loop->phi_count; ++index) {
    const ql_loop_phi_view &phi = loop->phis[index];
    EXPECT_EQ(loop->latch_count, phi.latch_value_count);
    EXPECT_NE(QL_IR_INVALID_BLOCK_ID, phi.entry_block);
    EXPECT_NE(QL_IR_INVALID_VALUE_ID, phi.entry_value);
    switch (phi.recurrence) {
    case QL_LOOP_RECURRENCE_ADD_CONSTANT:
      saw_add_one = saw_add_one || phi.step == 1u;
      break;
    case QL_LOOP_RECURRENCE_SUB_CONSTANT:
      saw_sub_two = saw_sub_two || phi.step == 2u;
      break;
    case QL_LOOP_RECURRENCE_AFFINE_MUL_ADD:
      saw_affine = saw_affine ||
                   (phi.multiplier == 3u && phi.offset == 4u);
      break;
    case QL_LOOP_RECURRENCE_POINTER_STRIDE:
      saw_pointer_stride = saw_pointer_stride || phi.step == 4u;
      break;
    default:
      break;
    }
  }
  EXPECT_TRUE(saw_add_one);
  EXPECT_TRUE(saw_sub_two);
  EXPECT_TRUE(saw_affine);
  EXPECT_TRUE(saw_pointer_stride);
}

TEST(LoopAnalysis, KeepsBodySideExitOutOfPreTestGuardFastPath) {
  constexpr char source[] =
      "int body_break(int x) {\n"
      "  for (;;) {\n"
      "    ++x;\n"
      "    if (x == 10) break;\n"
      "  }\n"
      "  return x;\n"
      "}\n";
  AnalyzedFunction function;
  ASSERT_TRUE(function.Open(source, "body_break"));
  ASSERT_EQ(1u, function.view()->loop_count);
  const ql_loop_view *loop = function.loop(0u);
  ASSERT_NE(nullptr, loop);
  EXPECT_EQ(QL_LOOP_GUARD_AMBIGUOUS, loop->guard.position);
  EXPECT_EQ(1u, loop->exit_count);
}

TEST(LoopAnalysis, MatchesPromotedNarrowInductionVariable) {
  constexpr char source[] =
      "int narrow_iv(unsigned char n) {\n"
      "  unsigned char i = 0;\n"
      "  int sum = 0;\n"
      "  while (i < n) { sum += i; ++i; }\n"
      "  return sum;\n"
      "}\n";
  AnalyzedFunction function;
  ASSERT_TRUE(function.Open(source, "narrow_iv"));
  ASSERT_EQ(1u, function.view()->loop_count);
  const ql_loop_view *loop = function.loop(0u);
  ASSERT_NE(nullptr, loop);

  bool saw_narrow_add_one = false;
  for (std::size_t index = 0u; index < loop->phi_count; ++index) {
    const ql_loop_phi_view &phi = loop->phis[index];
    if (phi.bit_width == 8u &&
        phi.recurrence == QL_LOOP_RECURRENCE_ADD_CONSTANT &&
        phi.step == 1u) {
      saw_narrow_add_one = true;
    }
  }
  EXPECT_TRUE(saw_narrow_add_one);
}

TEST(LoopAnalysis, RequiresOneRecurrenceAcrossEveryLatch) {
  constexpr char source[] =
      "int two_latches(int n) {\n"
      "  int i = 0;\n"
      "  int sum = 0;\n"
      "  while (i < n) {\n"
      "    ++i;\n"
      "    if (i & 1) continue;\n"
      "    sum += i;\n"
      "  }\n"
      "  return sum;\n"
      "}\n";
  AnalyzedFunction function;
  ASSERT_TRUE(function.Open(source, "two_latches"));
  ASSERT_EQ(1u, function.view()->loop_count);
  const ql_loop_view *loop = function.loop(0u);
  ASSERT_NE(nullptr, loop);
  ASSERT_EQ(2u, loop->latch_count);

  bool saw_common_add_one = false;
  bool saw_nonuniform_update = false;
  for (std::size_t index = 0u; index < loop->phi_count; ++index) {
    const ql_loop_phi_view &phi = loop->phis[index];
    ASSERT_EQ(2u, phi.latch_value_count);
    EXPECT_NE(QL_IR_INVALID_VALUE_ID, phi.latch_values[0]);
    EXPECT_NE(QL_IR_INVALID_VALUE_ID, phi.latch_values[1]);
    saw_common_add_one =
        saw_common_add_one ||
        (phi.recurrence == QL_LOOP_RECURRENCE_ADD_CONSTANT && phi.step == 1u);
    saw_nonuniform_update =
        saw_nonuniform_update || phi.recurrence == QL_LOOP_RECURRENCE_UNKNOWN;
  }
  EXPECT_TRUE(saw_common_add_one);
  EXPECT_TRUE(saw_nonuniform_update);
}

TEST(LoopAnalysis, FlagsMemoryTraceAndInstructionEffects) {
  constexpr char memory_source[] =
      "int memory_loop(int *p, int n) {\n"
      "  int i = 0;\n"
      "  while (i < n) { p[i] = i; ++i; }\n"
      "  return i;\n"
      "}\n";
  constexpr char trace_source[] =
      "extern int observe(int);\n"
      "int trace_loop(int n) {\n"
      "  int i = 0;\n"
      "  int sum = 0;\n"
      "  while (i < n) { sum += observe(i); ++i; }\n"
      "  return sum;\n"
      "}\n";
  AnalyzedFunction memory;
  AnalyzedFunction trace;
  ASSERT_TRUE(memory.Open(memory_source, "memory_loop"));
  ASSERT_TRUE(trace.Open(trace_source, "trace_loop"));
  ASSERT_EQ(1u, memory.view()->loop_count);
  ASSERT_EQ(1u, trace.view()->loop_count);
  const ql_loop_view *memory_loop = memory.loop(0u);
  const ql_loop_view *trace_loop = trace.loop(0u);
  ASSERT_NE(nullptr, memory_loop);
  ASSERT_NE(nullptr, trace_loop);
  EXPECT_EQ(1u, memory_loop->has_memory_state);
  EXPECT_NE(0u, memory_loop->effects & QL_IR_EFFECT_MEMORY);
  EXPECT_EQ(1u, trace_loop->has_event_trace_state);
  EXPECT_NE(0u, trace_loop->effects & QL_IR_EFFECT_CALL);
}

class FaultAllocator {
public:
  ql_allocator Make() {
    ql_allocator allocator{};
    allocator.user_data = this;
    allocator.allocate = Allocate;
    allocator.reallocate = Reallocate;
    allocator.deallocate = Deallocate;
    return allocator;
  }

  std::size_t fail_at = SIZE_MAX;
  std::size_t calls = 0u;
  std::size_t live = 0u;

private:
  static void *QL_CALL Allocate(void *user_data, std::size_t size) {
    auto *self = static_cast<FaultAllocator *>(user_data);
    const std::size_t call = self->calls++;
    if (call == self->fail_at) {
      return nullptr;
    }
    void *memory = std::malloc(size);
    if (memory != nullptr) {
      ++self->live;
    }
    return memory;
  }

  static void *QL_CALL Reallocate(void *user_data, void *pointer,
                                  std::size_t size) {
    auto *self = static_cast<FaultAllocator *>(user_data);
    const std::size_t call = self->calls++;
    if (call == self->fail_at) {
      return nullptr;
    }
    if (pointer == nullptr) {
      void *memory = std::malloc(size);
      if (memory != nullptr) {
        ++self->live;
      }
      return memory;
    }
    return std::realloc(pointer, size);
  }

  static void QL_CALL Deallocate(void *user_data, void *pointer) {
    auto *self = static_cast<FaultAllocator *>(user_data);
    if (pointer != nullptr) {
      ASSERT_GT(self->live, 0u);
      --self->live;
    }
    std::free(pointer);
  }
};

TEST(LoopAnalysis, CleansEveryPartialAllocationOnOutOfMemory) {
  constexpr char source[] =
      "int allocation_loop(int n) {\n"
      "  int i = 0;\n"
      "  while (i < n) ++i;\n"
      "  return i;\n"
      "}\n";
  AnalyzedFunction function;
  ASSERT_TRUE(function.Open(source, "allocation_loop"));

  FaultAllocator baseline;
  ql_allocator baseline_api = baseline.Make();
  ql_loop_analysis *analysis = nullptr;
  ql_error error{};
  ASSERT_EQ(QL_STATUS_OK, ql_loop_analysis_create(&baseline_api, function.ir(),
                                                  &analysis, &error))
      << error.message;
  ql_loop_analysis_destroy(analysis);
  ASSERT_EQ(0u, baseline.live);
  const std::size_t allocation_count = baseline.calls;
  ASSERT_GT(allocation_count, 0u);

  for (std::size_t fail_at = 0u; fail_at < allocation_count; ++fail_at) {
    FaultAllocator fault;
    fault.fail_at = fail_at;
    ql_allocator allocator = fault.Make();
    analysis = nullptr;
    std::memset(&error, 0, sizeof(error));
    EXPECT_EQ(QL_STATUS_OUT_OF_MEMORY,
              ql_loop_analysis_create(&allocator, function.ir(), &analysis,
                                      &error))
        << "allocation " << fail_at;
    EXPECT_EQ(nullptr, analysis) << "allocation " << fail_at;
    EXPECT_EQ(0u, fault.live) << "allocation " << fail_at;
  }
}

} // namespace
