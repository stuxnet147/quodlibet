#ifndef QUODLIBET_LOOP_ANALYSIS_H
#define QUODLIBET_LOOP_ANALYSIS_H

#include "quodlibet/allocator.h"
#include "quodlibet/ir.h"

QL_EXTERN_C_BEGIN

#define QL_LOOP_INVALID_INDEX SIZE_MAX

typedef struct ql_loop_analysis ql_loop_analysis;

/* The analyzer retains `ir`. Every pointer returned below is borrowed from
   the analysis object and remains valid until ql_loop_analysis_destroy.
   Block/edge arrays use deterministic numeric order for serialization and
   diagnostics only; block ids and source labels are not pairing evidence. */

typedef enum ql_loop_guard_position {
  QL_LOOP_GUARD_NONE = 0,
  QL_LOOP_GUARD_PRE_TEST,
  QL_LOOP_GUARD_POST_TEST,
  QL_LOOP_GUARD_AMBIGUOUS
} ql_loop_guard_position;

typedef enum ql_loop_recurrence_kind {
  QL_LOOP_RECURRENCE_UNKNOWN = 0,
  QL_LOOP_RECURRENCE_IDENTITY,
  QL_LOOP_RECURRENCE_ADD_CONSTANT,
  QL_LOOP_RECURRENCE_SUB_CONSTANT,
  QL_LOOP_RECURRENCE_AFFINE_MUL_ADD,
  QL_LOOP_RECURRENCE_POINTER_STRIDE
} ql_loop_recurrence_kind;

typedef struct ql_loop_edge {
  ql_ir_block_id from;
  ql_ir_block_id to;
} ql_loop_edge;

/* One leading PHI in the natural-loop header. latch_values is in the same
   canonical block-id order as ql_loop_view.latches. Constants are raw
   fixed-width bit-vector values. For ADD/SUB, step is the source constant; for
   affine recurrence multiplier and offset state A*x+B modulo bit_width; for a
   pointer recurrence step is the byte stride modulo the pointer width. Entry
   fields are invalid unless exactly one outside predecessor supplies the PHI.
   A recurrence is UNKNOWN unless every latch has the same classified update. */
typedef struct ql_loop_phi_view {
  ql_ir_instruction_id instruction;
  ql_ir_value_id result;
  ql_ir_type_id type;
  ql_ir_type_kind type_kind;
  uint32_t bit_width;
  ql_ir_block_id entry_block;
  ql_ir_value_id entry_value;
  const ql_ir_value_id *latch_values;
  size_t latch_value_count;
  ql_loop_recurrence_kind recurrence;
  uint64_t step;
  uint64_t multiplier;
  uint64_t offset;
} ql_loop_phi_view;

typedef struct ql_loop_guard_view {
  ql_loop_guard_position position;
  ql_ir_block_id block;
  ql_ir_value_id condition;
  ql_ir_block_id continue_target;
  ql_ir_block_id exit_target;
  /* Nonzero means condition=true continues the loop. A relational proof must
     establish that the two paired continue predicates agree; checking only
     both-continue and both-exit states leaves mismatched guards uncovered. */
  uint32_t continue_on_true;
} ql_loop_guard_view;

typedef struct ql_loop_view {
  size_t index;
  ql_ir_block_id header;
  /* Valid only for one outside predecessor whose sole successor is header. */
  ql_ir_block_id preheader;
  const ql_ir_block_id *blocks;
  size_t block_count;
  const ql_ir_block_id *latches;
  size_t latch_count;
  const ql_loop_edge *exits;
  size_t exit_count;
  const ql_loop_phi_view *phis;
  size_t phi_count;
  ql_loop_guard_view guard;
  size_t parent;
  size_t nesting_depth;
  uint32_t is_reducible;
  uint32_t is_single_entry;
  uint32_t is_reducible_single_entry;
  uint32_t has_memory_state;
  uint32_t has_event_trace_state;
  uint64_t effects;
} ql_loop_view;

typedef struct ql_loop_analysis_view {
  size_t block_count;
  size_t edge_count;
  size_t natural_backedge_count;
  size_t loop_count;
  uint32_t declared_cyclic;
  uint32_t has_actual_cycle;
  /* A cycle remains after removing every dominance backedge. Such a CFG is
     outside the reducible natural-loop fast path. */
  uint32_t has_unclassified_cycle;
} ql_loop_analysis_view;

ql_status ql_loop_analysis_create(const ql_allocator *allocator,
                                  const ql_ir *ir,
                                  ql_loop_analysis **output,
                                  ql_error *error);
void ql_loop_analysis_destroy(ql_loop_analysis *analysis);
const ql_loop_analysis_view *
ql_loop_analysis_get_view(const ql_loop_analysis *analysis);
const ql_loop_view *ql_loop_analysis_loop_at(const ql_loop_analysis *analysis,
                                             size_t index);
uint32_t ql_loop_analysis_contains_block(const ql_loop_analysis *analysis,
                                         size_t loop_index,
                                         ql_ir_block_id block);

QL_EXTERN_C_END

#endif
