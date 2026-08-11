#include "loop_analysis.h"

#include <stdlib.h>
#include <string.h>

typedef struct loop_cfg_edge {
  ql_ir_block_id from;
  ql_ir_block_id to;
} loop_cfg_edge;

typedef struct loop_storage {
  ql_loop_view view;
  uint8_t *membership;
} loop_storage;

struct ql_loop_analysis {
  ql_allocator allocator;
  const ql_ir *ir;
  ql_loop_analysis_view view;
  ql_ir_view_v1 ir_view;
  ql_ir_type_view_v1 *types;
  ql_ir_value_view_v1 *values;
  ql_ir_instruction_view_v1 *instructions;
  ql_ir_block_view_v1 *blocks;
  size_t *successor_counts;
  ql_ir_block_id *successors;
  loop_cfg_edge *edges;
  uint8_t *edge_is_backedge;
  size_t *predecessor_offsets;
  ql_ir_block_id *predecessors;
  ql_ir_block_id *order;
  uint64_t *dominators;
  size_t dominator_words;
  loop_storage *loops;
};

typedef struct recurrence_value {
  ql_loop_recurrence_kind kind;
  uint64_t step;
  uint64_t multiplier;
  uint64_t offset;
} recurrence_value;

typedef struct linear_form {
  uint64_t multiplier;
  uint64_t offset;
} linear_form;

static const ql_allocator *resolve_allocator(const ql_allocator *allocator) {
  if (allocator != NULL && ql_allocator_is_valid(allocator) != 0u) {
    return allocator;
  }
  return ql_default_allocator();
}

static int checked_multiply(size_t left, size_t right, size_t *output) {
  if (left != 0u && right > SIZE_MAX / left) {
    return 0;
  }
  *output = left * right;
  return 1;
}

static int checked_add(size_t left, size_t right, size_t *output) {
  if (right > SIZE_MAX - left) {
    return 0;
  }
  *output = left + right;
  return 1;
}

static ql_status allocate_zeroed(const ql_allocator *allocator, size_t count,
                                 size_t item_size, void **output,
                                 ql_error *error) {
  size_t size;
  void *memory;

  *output = NULL;
  if (count == 0u) {
    return QL_STATUS_OK;
  }
  if (!checked_multiply(count, item_size, &size)) {
    ql_error_set(error, QL_STATUS_OUT_OF_MEMORY,
                 "loop analysis allocation size overflows size_t");
    return QL_STATUS_OUT_OF_MEMORY;
  }
  memory = allocator->allocate(allocator->user_data, size);
  if (memory == NULL) {
    ql_error_set(error, QL_STATUS_OUT_OF_MEMORY, NULL);
    return QL_STATUS_OUT_OF_MEMORY;
  }
  memset(memory, 0, size);
  *output = memory;
  return QL_STATUS_OK;
}

static uint64_t width_mask(uint32_t width) {
  if (width >= 64u) {
    return UINT64_MAX;
  }
  if (width == 0u) {
    return UINT64_C(0);
  }
  return (UINT64_C(1) << width) - UINT64_C(1);
}

static size_t successor_count(const ql_ir_terminator_definition_v1 *term) {
  if (term->kind == QL_IR_TERMINATOR_BRANCH) {
    return 1u;
  }
  if (term->kind == QL_IR_TERMINATOR_COND_BRANCH) {
    return 2u;
  }
  return 0u;
}

static ql_ir_block_id successor_at(
    const ql_ir_terminator_definition_v1 *term, size_t index) {
  return index == 0u ? term->target : term->false_target;
}

static int edge_compare(const void *left_pointer, const void *right_pointer) {
  const ql_loop_edge *left = (const ql_loop_edge *)left_pointer;
  const ql_loop_edge *right = (const ql_loop_edge *)right_pointer;
  if (left->from < right->from) {
    return -1;
  }
  if (left->from > right->from) {
    return 1;
  }
  if (left->to < right->to) {
    return -1;
  }
  return left->to > right->to ? 1 : 0;
}

static int block_compare(const void *left_pointer, const void *right_pointer) {
  const ql_ir_block_id left = *(const ql_ir_block_id *)left_pointer;
  const ql_ir_block_id right = *(const ql_ir_block_id *)right_pointer;
  return left < right ? -1 : (left > right ? 1 : 0);
}

static ql_status cache_ir_views(ql_loop_analysis *analysis, ql_error *error) {
  size_t index;
  ql_status status;

  memset(&analysis->ir_view, 0, sizeof(analysis->ir_view));
  analysis->ir_view.struct_size = sizeof(analysis->ir_view);
  status = ql_ir_get_view(analysis->ir, &analysis->ir_view, error);
  if (status != QL_STATUS_OK) {
    return status;
  }
  analysis->view.block_count = analysis->ir_view.block_count;
  analysis->view.declared_cyclic =
      analysis->ir_view.cfg_kind == QL_IR_CFG_CYCLIC;

  status = allocate_zeroed(&analysis->allocator, analysis->ir_view.type_count,
                           sizeof(*analysis->types),
                           (void **)&analysis->types, error);
  if (status == QL_STATUS_OK) {
    status = allocate_zeroed(&analysis->allocator,
                             analysis->ir_view.value_count,
                             sizeof(*analysis->values),
                             (void **)&analysis->values, error);
  }
  if (status == QL_STATUS_OK) {
    status = allocate_zeroed(&analysis->allocator,
                             analysis->ir_view.instruction_count,
                             sizeof(*analysis->instructions),
                             (void **)&analysis->instructions, error);
  }
  if (status == QL_STATUS_OK) {
    status = allocate_zeroed(&analysis->allocator,
                             analysis->ir_view.block_count,
                             sizeof(*analysis->blocks),
                             (void **)&analysis->blocks, error);
  }
  if (status != QL_STATUS_OK) {
    return status;
  }

  for (index = 0u; index < analysis->ir_view.type_count; ++index) {
    analysis->types[index].struct_size = sizeof(analysis->types[index]);
    status = ql_ir_type_at(analysis->ir, index, &analysis->types[index], error);
    if (status != QL_STATUS_OK) {
      return status;
    }
  }
  for (index = 0u; index < analysis->ir_view.value_count; ++index) {
    analysis->values[index].struct_size = sizeof(analysis->values[index]);
    status = ql_ir_value_at(analysis->ir, index, &analysis->values[index], error);
    if (status != QL_STATUS_OK) {
      return status;
    }
  }
  for (index = 0u; index < analysis->ir_view.instruction_count; ++index) {
    analysis->instructions[index].struct_size =
        sizeof(analysis->instructions[index]);
    status = ql_ir_instruction_at(analysis->ir, index,
                                  &analysis->instructions[index], error);
    if (status != QL_STATUS_OK) {
      return status;
    }
  }
  for (index = 0u; index < analysis->ir_view.block_count; ++index) {
    analysis->blocks[index].struct_size = sizeof(analysis->blocks[index]);
    status = ql_ir_block_at(analysis->ir, index, &analysis->blocks[index], error);
    if (status != QL_STATUS_OK) {
      return status;
    }
  }
  return QL_STATUS_OK;
}

static ql_status build_cfg(ql_loop_analysis *analysis, ql_error *error) {
  size_t block_count = analysis->ir_view.block_count;
  size_t successor_capacity;
  size_t predecessor_offset_count;
  size_t edge_count = 0u;
  size_t index;
  size_t cursor;
  size_t *predecessor_cursor = NULL;
  ql_status status;

  if (!checked_multiply(block_count, 2u, &successor_capacity) ||
      !checked_add(block_count, 1u, &predecessor_offset_count)) {
    ql_error_set(error, QL_STATUS_OUT_OF_MEMORY,
                 "loop CFG size overflows size_t");
    return QL_STATUS_OUT_OF_MEMORY;
  }
  status = allocate_zeroed(&analysis->allocator, block_count,
                           sizeof(*analysis->successor_counts),
                           (void **)&analysis->successor_counts, error);
  if (status == QL_STATUS_OK) {
    status = allocate_zeroed(&analysis->allocator, successor_capacity,
                             sizeof(*analysis->successors),
                             (void **)&analysis->successors, error);
  }
  if (status != QL_STATUS_OK) {
    return status;
  }
  for (index = 0u; index < block_count; ++index) {
    const ql_ir_terminator_definition_v1 *term =
        &analysis->blocks[index].terminator;
    size_t nested;
    analysis->successor_counts[index] = successor_count(term);
    if (!checked_add(edge_count, analysis->successor_counts[index],
                     &edge_count)) {
      ql_error_set(error, QL_STATUS_OUT_OF_MEMORY,
                   "loop CFG edge count overflows size_t");
      return QL_STATUS_OUT_OF_MEMORY;
    }
    for (nested = 0u; nested < analysis->successor_counts[index]; ++nested) {
      analysis->successors[index * 2u + nested] = successor_at(term, nested);
    }
  }
  analysis->view.edge_count = edge_count;
  status = allocate_zeroed(&analysis->allocator, edge_count,
                           sizeof(*analysis->edges),
                           (void **)&analysis->edges, error);
  if (status == QL_STATUS_OK) {
    status = allocate_zeroed(&analysis->allocator, predecessor_offset_count,
                             sizeof(*analysis->predecessor_offsets),
                             (void **)&analysis->predecessor_offsets, error);
  }
  if (status == QL_STATUS_OK) {
    status = allocate_zeroed(&analysis->allocator, edge_count,
                             sizeof(*analysis->predecessors),
                             (void **)&analysis->predecessors, error);
  }
  if (status == QL_STATUS_OK) {
    status = allocate_zeroed(&analysis->allocator, block_count,
                             sizeof(*predecessor_cursor),
                             (void **)&predecessor_cursor, error);
  }
  if (status != QL_STATUS_OK) {
    analysis->allocator.deallocate(analysis->allocator.user_data,
                                   predecessor_cursor);
    return status;
  }

  cursor = 0u;
  for (index = 0u; index < block_count; ++index) {
    size_t nested;
    for (nested = 0u; nested < analysis->successor_counts[index]; ++nested) {
      ql_ir_block_id target = analysis->successors[index * 2u + nested];
      analysis->edges[cursor].from = (ql_ir_block_id)index;
      analysis->edges[cursor].to = target;
      ++predecessor_cursor[target];
      ++cursor;
    }
  }
  cursor = 0u;
  for (index = 0u; index < block_count; ++index) {
    size_t count = predecessor_cursor[index];
    analysis->predecessor_offsets[index] = cursor;
    predecessor_cursor[index] = 0u;
    cursor += count;
  }
  analysis->predecessor_offsets[block_count] = cursor;
  for (index = 0u; index < edge_count; ++index) {
    ql_ir_block_id target = analysis->edges[index].to;
    size_t slot = analysis->predecessor_offsets[target] +
                  predecessor_cursor[target]++;
    analysis->predecessors[slot] = analysis->edges[index].from;
  }
  analysis->allocator.deallocate(analysis->allocator.user_data,
                                 predecessor_cursor);
  return QL_STATUS_OK;
}

static ql_status build_reverse_postorder(ql_loop_analysis *analysis,
                                         ql_error *error) {
  size_t block_count = analysis->ir_view.block_count;
  ql_ir_block_id *stack = NULL;
  size_t *next = NULL;
  uint8_t *visited = NULL;
  size_t stack_size = 0u;
  size_t placed = 0u;
  size_t index;
  ql_status status;

  status = allocate_zeroed(&analysis->allocator, block_count,
                           sizeof(*analysis->order),
                           (void **)&analysis->order, error);
  if (status == QL_STATUS_OK) {
    status = allocate_zeroed(&analysis->allocator, block_count, sizeof(*stack),
                             (void **)&stack, error);
  }
  if (status == QL_STATUS_OK) {
    status = allocate_zeroed(&analysis->allocator, block_count, sizeof(*next),
                             (void **)&next, error);
  }
  if (status == QL_STATUS_OK) {
    status = allocate_zeroed(&analysis->allocator, block_count,
                             sizeof(*visited), (void **)&visited, error);
  }
  if (status != QL_STATUS_OK) {
    goto cleanup;
  }

  stack[stack_size++] = analysis->ir_view.entry_block;
  visited[analysis->ir_view.entry_block] = 1u;
  while (stack_size != 0u) {
    ql_ir_block_id block = stack[stack_size - 1u];
    if (next[block] < analysis->successor_counts[block]) {
      ql_ir_block_id target =
          analysis->successors[(size_t)block * 2u + next[block]++];
      if (visited[target] == 0u) {
        visited[target] = 1u;
        stack[stack_size++] = target;
      }
    } else {
      analysis->order[placed++] = block;
      --stack_size;
    }
  }
  if (placed != block_count) {
    ql_error_set(error, QL_STATUS_INVALID_ARGUMENT,
                 "loop analysis requires every IR block to be reachable");
    status = QL_STATUS_INVALID_ARGUMENT;
    goto cleanup;
  }
  for (index = 0u; index < placed / 2u; ++index) {
    ql_ir_block_id temporary = analysis->order[index];
    analysis->order[index] = analysis->order[placed - index - 1u];
    analysis->order[placed - index - 1u] = temporary;
  }

cleanup:
  analysis->allocator.deallocate(analysis->allocator.user_data, visited);
  analysis->allocator.deallocate(analysis->allocator.user_data, next);
  analysis->allocator.deallocate(analysis->allocator.user_data, stack);
  return status;
}

static uint64_t *dominator_row(ql_loop_analysis *analysis,
                               ql_ir_block_id block) {
  return analysis->dominators + (size_t)block * analysis->dominator_words;
}

static const uint64_t *const_dominator_row(const ql_loop_analysis *analysis,
                                           ql_ir_block_id block) {
  return analysis->dominators + (size_t)block * analysis->dominator_words;
}

static void bit_set(uint64_t *bits, size_t index) {
  bits[index / 64u] |= UINT64_C(1) << (index % 64u);
}

static int bit_test(const uint64_t *bits, size_t index) {
  return (bits[index / 64u] & (UINT64_C(1) << (index % 64u))) != 0u;
}

static int block_dominates(const ql_loop_analysis *analysis,
                           ql_ir_block_id dominator,
                           ql_ir_block_id block) {
  return bit_test(const_dominator_row(analysis, block), dominator);
}

static ql_status build_dominators(ql_loop_analysis *analysis,
                                  ql_error *error) {
  size_t block_count = analysis->ir_view.block_count;
  size_t dominator_count;
  size_t dominator_bytes;
  uint64_t *candidate = NULL;
  size_t index;
  int changed;
  ql_status status;

  analysis->dominator_words =
      block_count / 64u + (block_count % 64u != 0u ? 1u : 0u);
  if (!checked_multiply(block_count, analysis->dominator_words,
                        &dominator_count) ||
      !checked_multiply(dominator_count, sizeof(*analysis->dominators),
                        &dominator_bytes)) {
    ql_error_set(error, QL_STATUS_OUT_OF_MEMORY,
                 "dominator matrix size overflows size_t");
    return QL_STATUS_OUT_OF_MEMORY;
  }
  status = allocate_zeroed(&analysis->allocator, dominator_count,
                           sizeof(*analysis->dominators),
                           (void **)&analysis->dominators, error);
  if (status == QL_STATUS_OK) {
    status = allocate_zeroed(&analysis->allocator,
                             analysis->dominator_words, sizeof(*candidate),
                             (void **)&candidate, error);
  }
  if (status != QL_STATUS_OK) {
    analysis->allocator.deallocate(analysis->allocator.user_data, candidate);
    return status;
  }
  memset(analysis->dominators, 0xff, dominator_bytes);
  memset(dominator_row(analysis, analysis->ir_view.entry_block), 0,
         analysis->dominator_words * sizeof(*analysis->dominators));
  bit_set(dominator_row(analysis, analysis->ir_view.entry_block),
          analysis->ir_view.entry_block);

  do {
    changed = 0;
    for (index = 1u; index < block_count; ++index) {
      ql_ir_block_id block = analysis->order[index];
      size_t begin = analysis->predecessor_offsets[block];
      size_t end = analysis->predecessor_offsets[(size_t)block + 1u];
      size_t predecessor;
      size_t word;
      uint64_t *row = dominator_row(analysis, block);
      if (begin == end) {
        ql_error_set(error, QL_STATUS_INVALID_ARGUMENT,
                     "non-entry IR block %u has no predecessor", block);
        status = QL_STATUS_INVALID_ARGUMENT;
        goto cleanup;
      }
      memcpy(candidate, const_dominator_row(analysis,
                                             analysis->predecessors[begin]),
             analysis->dominator_words * sizeof(*candidate));
      for (predecessor = begin + 1u; predecessor < end; ++predecessor) {
        const uint64_t *other = const_dominator_row(
            analysis, analysis->predecessors[predecessor]);
        for (word = 0u; word < analysis->dominator_words; ++word) {
          candidate[word] &= other[word];
        }
      }
      bit_set(candidate, block);
      if (memcmp(row, candidate,
                 analysis->dominator_words * sizeof(*candidate)) != 0) {
        memcpy(row, candidate,
               analysis->dominator_words * sizeof(*candidate));
        changed = 1;
      }
    }
  } while (changed != 0);

cleanup:
  analysis->allocator.deallocate(analysis->allocator.user_data, candidate);
  return status;
}

static size_t kahn_count(const ql_loop_analysis *analysis,
                         int exclude_backedges, size_t *indegree,
                         ql_ir_block_id *queue) {
  size_t block_count = analysis->ir_view.block_count;
  size_t queue_begin = 0u;
  size_t queue_end = 0u;
  size_t index;

  memset(indegree, 0, block_count * sizeof(*indegree));
  for (index = 0u; index < analysis->view.edge_count; ++index) {
    if (exclude_backedges != 0 && analysis->edge_is_backedge[index] != 0u) {
      continue;
    }
    ++indegree[analysis->edges[index].to];
  }
  for (index = 0u; index < block_count; ++index) {
    if (indegree[index] == 0u) {
      queue[queue_end++] = (ql_ir_block_id)index;
    }
  }
  while (queue_begin < queue_end) {
    ql_ir_block_id block = queue[queue_begin++];
    for (index = 0u; index < analysis->view.edge_count; ++index) {
      ql_ir_block_id target;
      if (analysis->edges[index].from != block ||
          (exclude_backedges != 0 &&
           analysis->edge_is_backedge[index] != 0u)) {
        continue;
      }
      target = analysis->edges[index].to;
      if (--indegree[target] == 0u) {
        queue[queue_end++] = target;
      }
    }
  }
  return queue_end;
}

static ql_status classify_graph_cycles(ql_loop_analysis *analysis,
                                       ql_error *error) {
  size_t block_count = analysis->ir_view.block_count;
  size_t *indegree = NULL;
  ql_ir_block_id *queue = NULL;
  size_t index;
  ql_status status;

  status = allocate_zeroed(&analysis->allocator, analysis->view.edge_count,
                           sizeof(*analysis->edge_is_backedge),
                           (void **)&analysis->edge_is_backedge, error);
  if (status == QL_STATUS_OK) {
    status = allocate_zeroed(&analysis->allocator, block_count,
                             sizeof(*indegree), (void **)&indegree, error);
  }
  if (status == QL_STATUS_OK) {
    status = allocate_zeroed(&analysis->allocator, block_count, sizeof(*queue),
                             (void **)&queue, error);
  }
  if (status != QL_STATUS_OK) {
    goto cleanup;
  }
  for (index = 0u; index < analysis->view.edge_count; ++index) {
    if (block_dominates(analysis, analysis->edges[index].to,
                        analysis->edges[index].from)) {
      analysis->edge_is_backedge[index] = 1u;
      ++analysis->view.natural_backedge_count;
    }
  }
  analysis->view.has_actual_cycle =
      kahn_count(analysis, 0, indegree, queue) != block_count;
  analysis->view.has_unclassified_cycle =
      kahn_count(analysis, 1, indegree, queue) != block_count;

cleanup:
  analysis->allocator.deallocate(analysis->allocator.user_data, queue);
  analysis->allocator.deallocate(analysis->allocator.user_data, indegree);
  return status;
}

static int membership_contains(const loop_storage *loop,
                               ql_ir_block_id block) {
  return loop->membership[block] != 0u;
}

static size_t unique_latch_count(const ql_loop_analysis *analysis,
                                 ql_ir_block_id header) {
  size_t count = 0u;
  size_t index;
  ql_ir_block_id previous = QL_IR_INVALID_BLOCK_ID;

  /* Edges are in source-block order, so equal latch blocks are adjacent. */
  for (index = 0u; index < analysis->view.edge_count; ++index) {
    if (analysis->edge_is_backedge[index] == 0u ||
        analysis->edges[index].to != header ||
        analysis->edges[index].from == previous) {
      continue;
    }
    previous = analysis->edges[index].from;
    ++count;
  }
  return count;
}

static ql_status build_loop_membership(ql_loop_analysis *analysis,
                                       loop_storage *loop,
                                       ql_error *error) {
  size_t block_count = analysis->ir_view.block_count;
  ql_ir_block_id *stack = NULL;
  size_t stack_size = 0u;
  size_t index;
  size_t latch_at = 0u;
  size_t loop_block_count = 0u;
  ql_ir_block_id previous = QL_IR_INVALID_BLOCK_ID;
  ql_status status;

  status = allocate_zeroed(&analysis->allocator, block_count,
                           sizeof(*loop->membership),
                           (void **)&loop->membership, error);
  if (status == QL_STATUS_OK) {
    status = allocate_zeroed(&analysis->allocator, block_count, sizeof(*stack),
                             (void **)&stack, error);
  }
  loop->view.latch_count = unique_latch_count(analysis, loop->view.header);
  if (status == QL_STATUS_OK) {
    status = allocate_zeroed(&analysis->allocator, loop->view.latch_count,
                             sizeof(*loop->view.latches),
                             (void **)&loop->view.latches, error);
  }
  if (status != QL_STATUS_OK) {
    goto cleanup;
  }

  loop->membership[loop->view.header] = 1u;
  for (index = 0u; index < analysis->view.edge_count; ++index) {
    ql_ir_block_id latch;
    if (analysis->edge_is_backedge[index] == 0u ||
        analysis->edges[index].to != loop->view.header) {
      continue;
    }
    latch = analysis->edges[index].from;
    if (latch == previous) {
      continue;
    }
    previous = latch;
    ((ql_ir_block_id *)loop->view.latches)[latch_at++] = latch;
    if (loop->membership[latch] == 0u) {
      loop->membership[latch] = 1u;
      stack[stack_size++] = latch;
    }
  }
  qsort((void *)loop->view.latches, loop->view.latch_count,
        sizeof(*loop->view.latches), block_compare);
  while (stack_size != 0u) {
    ql_ir_block_id block = stack[--stack_size];
    size_t begin = analysis->predecessor_offsets[block];
    size_t end = analysis->predecessor_offsets[(size_t)block + 1u];
    size_t predecessor;
    for (predecessor = begin; predecessor < end; ++predecessor) {
      ql_ir_block_id incoming = analysis->predecessors[predecessor];
      if (loop->membership[incoming] == 0u) {
        loop->membership[incoming] = 1u;
        stack[stack_size++] = incoming;
      }
    }
  }
  for (index = 0u; index < block_count; ++index) {
    loop_block_count += loop->membership[index] != 0u;
  }
  loop->view.block_count = loop_block_count;
  status = allocate_zeroed(&analysis->allocator, loop_block_count,
                           sizeof(*loop->view.blocks),
                           (void **)&loop->view.blocks, error);
  if (status == QL_STATUS_OK) {
    size_t output = 0u;
    for (index = 0u; index < block_count; ++index) {
      if (loop->membership[index] != 0u) {
        ((ql_ir_block_id *)loop->view.blocks)[output++] =
            (ql_ir_block_id)index;
      }
    }
  }

cleanup:
  analysis->allocator.deallocate(analysis->allocator.user_data, stack);
  return status;
}

static size_t distinct_successor_count(const ql_loop_analysis *analysis,
                                       ql_ir_block_id block,
                                       ql_ir_block_id *only) {
  size_t count = analysis->successor_counts[block];
  if (count == 0u) {
    return 0u;
  }
  *only = analysis->successors[(size_t)block * 2u];
  if (count == 1u ||
      analysis->successors[(size_t)block * 2u + 1u] == *only) {
    return 1u;
  }
  return 2u;
}

static ql_status classify_loop_entry_and_exits(ql_loop_analysis *analysis,
                                               loop_storage *loop,
                                               ql_error *error) {
  size_t block_count = analysis->ir_view.block_count;
  size_t outside_entry_count = 0u;
  size_t header_predecessor_count = 0u;
  ql_ir_block_id unique_header_predecessor = QL_IR_INVALID_BLOCK_ID;
  ql_ir_block_id previous_header_predecessor = QL_IR_INVALID_BLOCK_ID;
  size_t exit_capacity = analysis->view.edge_count;
  ql_loop_edge *exits = NULL;
  size_t exit_count = 0u;
  size_t index;
  ql_status status = allocate_zeroed(&analysis->allocator, exit_capacity,
                                     sizeof(*exits), (void **)&exits, error);

  if (status != QL_STATUS_OK) {
    return status;
  }
  loop->view.is_reducible = 1u;
  loop->view.is_single_entry = 1u;
  for (index = 0u; index < block_count; ++index) {
    if (loop->membership[index] != 0u &&
        !block_dominates(analysis, loop->view.header,
                         (ql_ir_block_id)index)) {
      loop->view.is_reducible = 0u;
    }
  }
  for (index = 0u; index < analysis->view.edge_count; ++index) {
    const loop_cfg_edge edge = analysis->edges[index];
    const int from_inside = membership_contains(loop, edge.from);
    const int to_inside = membership_contains(loop, edge.to);
    if (!from_inside && to_inside) {
      if (edge.to != loop->view.header) {
        loop->view.is_single_entry = 0u;
      }
      ++outside_entry_count;
      if (edge.to == loop->view.header &&
          edge.from != previous_header_predecessor) {
        previous_header_predecessor = edge.from;
        unique_header_predecessor = edge.from;
        ++header_predecessor_count;
      }
    } else if (from_inside && !to_inside) {
      exits[exit_count].from = edge.from;
      exits[exit_count].to = edge.to;
      ++exit_count;
    }
  }
  (void)outside_entry_count;
  if (header_predecessor_count == 1u) {
    ql_ir_block_id only = QL_IR_INVALID_BLOCK_ID;
    if (distinct_successor_count(analysis, unique_header_predecessor, &only) ==
            1u &&
        only == loop->view.header) {
      loop->view.preheader = unique_header_predecessor;
    }
  }
  if (exit_count != 0u) {
    size_t output = 0u;
    qsort(exits, exit_count, sizeof(*exits), edge_compare);
    for (index = 0u; index < exit_count; ++index) {
      if (output == 0u || exits[index].from != exits[output - 1u].from ||
          exits[index].to != exits[output - 1u].to) {
        exits[output++] = exits[index];
      }
    }
    exit_count = output;
  }
  loop->view.exits = exits;
  loop->view.exit_count = exit_count;
  loop->view.is_reducible_single_entry =
      loop->view.is_reducible && loop->view.is_single_entry;
  return QL_STATUS_OK;
}

static int is_latch(const ql_loop_view *loop, ql_ir_block_id block) {
  size_t index;
  for (index = 0u; index < loop->latch_count; ++index) {
    if (loop->latches[index] == block) {
      return 1;
    }
  }
  return 0;
}

static void classify_guard(const ql_loop_analysis *analysis,
                           loop_storage *loop) {
  ql_loop_guard_view found;
  size_t candidate_count = 0u;
  size_t index;

  memset(&found, 0, sizeof(found));
  found.position = QL_LOOP_GUARD_NONE;
  found.block = QL_IR_INVALID_BLOCK_ID;
  found.condition = QL_IR_INVALID_VALUE_ID;
  found.continue_target = QL_IR_INVALID_BLOCK_ID;
  found.exit_target = QL_IR_INVALID_BLOCK_ID;
  for (index = 0u; index < loop->view.block_count; ++index) {
    ql_ir_block_id block = loop->view.blocks[index];
    const ql_ir_terminator_definition_v1 *term =
        &analysis->blocks[block].terminator;
    int target_inside;
    int false_inside;
    ql_loop_guard_view candidate;
    if (term->kind != QL_IR_TERMINATOR_COND_BRANCH) {
      continue;
    }
    target_inside = membership_contains(loop, term->target);
    false_inside = membership_contains(loop, term->false_target);
    if (target_inside == false_inside) {
      continue;
    }
    memset(&candidate, 0, sizeof(candidate));
    candidate.block = block;
    candidate.condition = term->condition;
    candidate.continue_on_true = target_inside != 0;
    candidate.continue_target =
        target_inside != 0 ? term->target : term->false_target;
    candidate.exit_target =
        target_inside != 0 ? term->false_target : term->target;
    if (block == loop->view.header) {
      candidate.position = QL_LOOP_GUARD_PRE_TEST;
    } else if (candidate.continue_target == loop->view.header &&
               is_latch(&loop->view, block)) {
      candidate.position = QL_LOOP_GUARD_POST_TEST;
    } else {
      /* An inside/outside branch in the body can be a break after state has
         already advanced. It is not safe to move that branch to the
         iteration-entry cutpoint merely because its CFG shape resembles a
         split condition block. Effect-split entry conditions and other
         non-canonical guards stay explicit fallback cases. */
      candidate.position = QL_LOOP_GUARD_AMBIGUOUS;
    }
    if (candidate_count == 0u) {
      found = candidate;
    }
    ++candidate_count;
  }
  if (candidate_count > 1u) {
    found.position = QL_LOOP_GUARD_AMBIGUOUS;
    found.block = QL_IR_INVALID_BLOCK_ID;
    found.condition = QL_IR_INVALID_VALUE_ID;
    found.continue_target = QL_IR_INVALID_BLOCK_ID;
    found.exit_target = QL_IR_INVALID_BLOCK_ID;
    found.continue_on_true = 0u;
  }
  loop->view.guard = found;
}

static const ql_ir_instruction_view_v1 *defining_instruction(
    const ql_loop_analysis *analysis, ql_ir_value_id value) {
  const ql_ir_value_view_v1 *entry;
  if (value >= analysis->ir_view.value_count) {
    return NULL;
  }
  entry = &analysis->values[value];
  if (entry->definition_kind != QL_IR_VALUE_INSTRUCTION_RESULT ||
      entry->instruction >= analysis->ir_view.instruction_count) {
    return NULL;
  }
  return &analysis->instructions[entry->instruction];
}

static uint32_t value_bit_width(const ql_loop_analysis *analysis,
                                ql_ir_value_id value) {
  ql_ir_type_id type;
  const ql_ir_type_view_v1 *view;
  if (value >= analysis->ir_view.value_count) {
    return 0u;
  }
  type = analysis->values[value].type;
  if (type >= analysis->ir_view.type_count) {
    return 0u;
  }
  view = &analysis->types[type];
  return view->kind == QL_IR_TYPE_BOOL ? 1u : view->bit_width;
}

static ql_ir_value_id strip_identity(const ql_loop_analysis *analysis,
                                     ql_ir_value_id value) {
  size_t depth;
  for (depth = 0u; depth < 32u; ++depth) {
    const ql_ir_instruction_view_v1 *instruction =
        defining_instruction(analysis, value);
    if (instruction == NULL || instruction->opcode != QL_IR_OPCODE_IDENTITY ||
        instruction->operand_count != 1u) {
      break;
    }
    value = instruction->operands[0];
  }
  return value;
}

static int constant_value(const ql_loop_analysis *analysis,
                          ql_ir_value_id value, size_t depth,
                          uint64_t *output) {
  const ql_ir_value_view_v1 *entry;
  const ql_ir_instruction_view_v1 *instruction;
  uint32_t width;
  uint64_t mask;
  uint64_t left;
  uint64_t right;
  size_t index;

  if (depth > 32u || value >= analysis->ir_view.value_count) {
    return 0;
  }
  entry = &analysis->values[value];
  width = value_bit_width(analysis, value);
  if (width == 0u || width > 64u) {
    return 0;
  }
  mask = width_mask(width);
  if (entry->definition_kind == QL_IR_VALUE_CONSTANT) {
    if (entry->constant_data == NULL || entry->constant_size == 0u ||
        entry->constant_size > 8u) {
      return 0;
    }
    *output = 0u;
    for (index = 0u; index < entry->constant_size; ++index) {
      *output |= (uint64_t)((const uint8_t *)entry->constant_data)[index]
                 << (index * 8u);
    }
    *output &= mask;
    return 1;
  }
  instruction = defining_instruction(analysis, value);
  if (instruction == NULL) {
    return 0;
  }
  if ((instruction->opcode == QL_IR_OPCODE_IDENTITY ||
       instruction->opcode == QL_IR_OPCODE_ZEXT ||
       instruction->opcode == QL_IR_OPCODE_TRUNC ||
       instruction->opcode == QL_IR_OPCODE_BITCAST) &&
      instruction->operand_count == 1u &&
      constant_value(analysis, instruction->operands[0], depth + 1u, &left)) {
    *output = left & mask;
    return 1;
  }
  if (instruction->opcode == QL_IR_OPCODE_SEXT &&
      instruction->operand_count == 1u &&
      constant_value(analysis, instruction->operands[0], depth + 1u, &left)) {
    uint32_t source_width = value_bit_width(analysis, instruction->operands[0]);
    if (source_width != 0u && source_width < 64u &&
        (left & (UINT64_C(1) << (source_width - 1u))) != 0u) {
      left |= ~width_mask(source_width);
    }
    *output = left & mask;
    return 1;
  }
  if (instruction->opcode == QL_IR_OPCODE_BV_NEG &&
      instruction->operand_count == 1u &&
      constant_value(analysis, instruction->operands[0], depth + 1u, &left)) {
    *output = (UINT64_C(0) - left) & mask;
    return 1;
  }
  if (instruction->operand_count != 2u ||
      !constant_value(analysis, instruction->operands[0], depth + 1u, &left) ||
      !constant_value(analysis, instruction->operands[1], depth + 1u, &right)) {
    return 0;
  }
  switch (instruction->opcode) {
  case QL_IR_OPCODE_ADD:
    *output = (left + right) & mask;
    return 1;
  case QL_IR_OPCODE_SUB:
    *output = (left - right) & mask;
    return 1;
  case QL_IR_OPCODE_MUL:
    *output = (left * right) & mask;
    return 1;
  default:
    return 0;
  }
}

static int same_phi_value(const ql_loop_analysis *analysis,
                          ql_ir_value_id value, ql_ir_value_id phi) {
  return strip_identity(analysis, value) == phi;
}

static int linear_expression(const ql_loop_analysis *analysis,
                             ql_ir_value_id value, ql_ir_value_id phi,
                             uint32_t width, size_t depth,
                             linear_form *output) {
  const ql_ir_instruction_view_v1 *instruction;
  linear_form left;
  linear_form right;
  uint64_t constant;
  uint64_t mask = width_mask(width);

  if (depth > 32u || width == 0u || width > 64u) {
    return 0;
  }
  value = strip_identity(analysis, value);
  if (value == phi) {
    output->multiplier = 1u;
    output->offset = 0u;
    return 1;
  }
  if (constant_value(analysis, value, 0u, &constant)) {
    output->multiplier = 0u;
    output->offset = constant & mask;
    return 1;
  }
  instruction = defining_instruction(analysis, value);
  if (instruction != NULL && instruction->operand_count == 1u &&
      (instruction->opcode == QL_IR_OPCODE_ZEXT ||
       instruction->opcode == QL_IR_OPCODE_SEXT ||
       instruction->opcode == QL_IR_OPCODE_TRUNC ||
       instruction->opcode == QL_IR_OPCODE_BITCAST)) {
    const uint32_t source_width =
        value_bit_width(analysis, instruction->operands[0]);
    const uint32_t result_width = value_bit_width(analysis, value);
    /* These casts preserve the low `width` bits only when neither side is
       narrower than the carried state. This recognizes the usual C integer
       promotion followed by assignment truncation without treating an
       arbitrary truncate-and-reextend as identity. */
    if (source_width >= width && result_width >= width) {
      return linear_expression(analysis, instruction->operands[0], phi, width,
                               depth + 1u, output);
    }
    return 0;
  }
  if (instruction == NULL || instruction->operand_count != 2u ||
      (instruction->opcode != QL_IR_OPCODE_ADD &&
       instruction->opcode != QL_IR_OPCODE_SUB &&
       instruction->opcode != QL_IR_OPCODE_MUL)) {
    return 0;
  }
  if (!linear_expression(analysis, instruction->operands[0], phi, width,
                         depth + 1u, &left) ||
      !linear_expression(analysis, instruction->operands[1], phi, width,
                         depth + 1u, &right)) {
    return 0;
  }
  if (instruction->opcode == QL_IR_OPCODE_ADD) {
    output->multiplier = (left.multiplier + right.multiplier) & mask;
    output->offset = (left.offset + right.offset) & mask;
    return 1;
  }
  if (instruction->opcode == QL_IR_OPCODE_SUB) {
    output->multiplier = (left.multiplier - right.multiplier) & mask;
    output->offset = (left.offset - right.offset) & mask;
    return 1;
  }
  if (left.multiplier != 0u && right.multiplier != 0u) {
    return 0;
  }
  output->multiplier =
      (left.multiplier * right.offset + right.multiplier * left.offset) & mask;
  output->offset = (left.offset * right.offset) & mask;
  return 1;
}

static recurrence_value classify_recurrence(
    const ql_loop_analysis *analysis, ql_ir_value_id phi,
    ql_ir_type_kind type_kind, uint32_t width, ql_ir_value_id next) {
  recurrence_value result;
  const ql_ir_instruction_view_v1 *instruction;
  uint64_t constant;
  linear_form linear;

  memset(&result, 0, sizeof(result));
  result.kind = QL_LOOP_RECURRENCE_UNKNOWN;
  next = strip_identity(analysis, next);
  if (next == phi) {
    result.kind = QL_LOOP_RECURRENCE_IDENTITY;
    result.multiplier = 1u;
    return result;
  }
  instruction = defining_instruction(analysis, next);
  if (instruction == NULL) {
    return result;
  }
  if (type_kind == QL_IR_TYPE_POINTER &&
      instruction->opcode == QL_IR_OPCODE_PTR_ADD &&
      instruction->operand_count == 2u &&
      next < analysis->ir_view.value_count &&
      analysis->values[next].type == analysis->values[phi].type &&
      value_bit_width(analysis, instruction->operands[1]) == width &&
      same_phi_value(analysis, instruction->operands[0], phi) &&
      constant_value(analysis, instruction->operands[1], 0u, &constant)) {
    result.kind = QL_LOOP_RECURRENCE_POINTER_STRIDE;
    result.step = constant & width_mask(width);
    result.multiplier = 1u;
    result.offset = result.step;
    return result;
  }
  if (type_kind != QL_IR_TYPE_BIT_VECTOR || width == 0u || width > 64u) {
    return result;
  }
  if (instruction->opcode == QL_IR_OPCODE_ADD &&
      instruction->operand_count == 2u) {
    if (same_phi_value(analysis, instruction->operands[0], phi) &&
        constant_value(analysis, instruction->operands[1], 0u, &constant)) {
      result.kind = QL_LOOP_RECURRENCE_ADD_CONSTANT;
    } else if (same_phi_value(analysis, instruction->operands[1], phi) &&
               constant_value(analysis, instruction->operands[0], 0u,
                              &constant)) {
      result.kind = QL_LOOP_RECURRENCE_ADD_CONSTANT;
    }
    if (result.kind == QL_LOOP_RECURRENCE_ADD_CONSTANT) {
      result.step = constant & width_mask(width);
      result.multiplier = 1u;
      result.offset = result.step;
      return result;
    }
  }
  if (instruction->opcode == QL_IR_OPCODE_SUB &&
      instruction->operand_count == 2u &&
      same_phi_value(analysis, instruction->operands[0], phi) &&
      constant_value(analysis, instruction->operands[1], 0u, &constant)) {
    result.kind = QL_LOOP_RECURRENCE_SUB_CONSTANT;
    result.step = constant & width_mask(width);
    result.multiplier = 1u;
    result.offset = (UINT64_C(0) - result.step) & width_mask(width);
    return result;
  }
  if (linear_expression(analysis, next, phi, width, 0u, &linear)) {
    result.multiplier = linear.multiplier;
    result.offset = linear.offset;
    if (linear.multiplier == 1u && linear.offset == 0u) {
      result.kind = QL_LOOP_RECURRENCE_IDENTITY;
    } else if (linear.multiplier == 1u) {
      result.kind = QL_LOOP_RECURRENCE_ADD_CONSTANT;
      result.step = linear.offset;
    } else {
      result.kind = QL_LOOP_RECURRENCE_AFFINE_MUL_ADD;
    }
  }
  return result;
}

static int recurrence_equal(recurrence_value left, recurrence_value right) {
  return left.kind == right.kind && left.step == right.step &&
         left.multiplier == right.multiplier && left.offset == right.offset;
}

static ql_status build_loop_phis(ql_loop_analysis *analysis,
                                 loop_storage *loop, ql_error *error) {
  const ql_ir_block_view_v1 *header = &analysis->blocks[loop->view.header];
  size_t phi_count = 0u;
  size_t index;
  ql_loop_phi_view *phis;
  ql_status status;

  while (phi_count < header->instruction_count &&
         analysis->instructions[header->instructions[phi_count]].opcode ==
             QL_IR_OPCODE_PHI) {
    ++phi_count;
  }
  status = allocate_zeroed(&analysis->allocator, phi_count, sizeof(*phis),
                           (void **)&phis, error);
  if (status != QL_STATUS_OK) {
    return status;
  }
  loop->view.phis = phis;
  loop->view.phi_count = phi_count;
  for (index = 0u; index < phi_count; ++index) {
    const ql_ir_instruction_view_v1 *instruction =
        &analysis->instructions[header->instructions[index]];
    ql_loop_phi_view *phi = &phis[index];
    const ql_ir_value_view_v1 *result;
    const ql_ir_type_view_v1 *type;
    size_t incoming;
    size_t latch;
    size_t entry_count = 0u;
    recurrence_value common;
    int has_common = 0;

    phi->instruction = instruction->id;
    phi->result = instruction->result_count == 1u
                      ? instruction->results[0]
                      : QL_IR_INVALID_VALUE_ID;
    phi->entry_block = QL_IR_INVALID_BLOCK_ID;
    phi->entry_value = QL_IR_INVALID_VALUE_ID;
    phi->recurrence = QL_LOOP_RECURRENCE_UNKNOWN;
    phi->latch_value_count = loop->view.latch_count;
    status = allocate_zeroed(&analysis->allocator, loop->view.latch_count,
                             sizeof(*phi->latch_values),
                             (void **)&phi->latch_values, error);
    if (status != QL_STATUS_OK) {
      return status;
    }
    for (latch = 0u; latch < loop->view.latch_count; ++latch) {
      ((ql_ir_value_id *)phi->latch_values)[latch] = QL_IR_INVALID_VALUE_ID;
    }
    if (phi->result == QL_IR_INVALID_VALUE_ID ||
        phi->result >= analysis->ir_view.value_count) {
      continue;
    }
    result = &analysis->values[phi->result];
    phi->type = result->type;
    if (phi->type >= analysis->ir_view.type_count) {
      continue;
    }
    type = &analysis->types[phi->type];
    phi->type_kind = type->kind;
    phi->bit_width = type->kind == QL_IR_TYPE_BOOL ? 1u : type->bit_width;
    if (type->kind == QL_IR_TYPE_MEMORY) {
      loop->view.has_memory_state = 1u;
    } else if (type->kind == QL_IR_TYPE_EVENT_TRACE) {
      loop->view.has_event_trace_state = 1u;
    }
    for (incoming = 0u; incoming < instruction->block_operand_count;
         ++incoming) {
      ql_ir_block_id block = instruction->block_operands[incoming];
      if (!membership_contains(loop, block)) {
        ++entry_count;
        if (entry_count == 1u) {
          phi->entry_block = block;
          phi->entry_value = instruction->operands[incoming];
        } else {
          phi->entry_block = QL_IR_INVALID_BLOCK_ID;
          phi->entry_value = QL_IR_INVALID_VALUE_ID;
        }
      }
      for (latch = 0u; latch < loop->view.latch_count; ++latch) {
        if (loop->view.latches[latch] == block) {
          ((ql_ir_value_id *)phi->latch_values)[latch] =
              instruction->operands[incoming];
          break;
        }
      }
    }
    memset(&common, 0, sizeof(common));
    for (latch = 0u; latch < loop->view.latch_count; ++latch) {
      recurrence_value current;
      if (phi->latch_values[latch] == QL_IR_INVALID_VALUE_ID) {
        has_common = 0;
        break;
      }
      current = classify_recurrence(analysis, phi->result, phi->type_kind,
                                    phi->bit_width,
                                    phi->latch_values[latch]);
      if (has_common == 0) {
        common = current;
        has_common = 1;
      } else if (!recurrence_equal(common, current)) {
        has_common = 0;
        break;
      }
    }
    if (has_common != 0) {
      phi->recurrence = common.kind;
      phi->step = common.step;
      phi->multiplier = common.multiplier;
      phi->offset = common.offset;
    }
  }
  return QL_STATUS_OK;
}

static void note_value_state(const ql_loop_analysis *analysis,
                             loop_storage *loop, ql_ir_value_id value) {
  ql_ir_type_id type;
  if (value == QL_IR_INVALID_VALUE_ID ||
      value >= analysis->ir_view.value_count) {
    return;
  }
  type = analysis->values[value].type;
  if (type >= analysis->ir_view.type_count) {
    return;
  }
  if (analysis->types[type].kind == QL_IR_TYPE_MEMORY) {
    loop->view.has_memory_state = 1u;
  } else if (analysis->types[type].kind == QL_IR_TYPE_EVENT_TRACE) {
    loop->view.has_event_trace_state = 1u;
  }
}

static void classify_loop_state(const ql_loop_analysis *analysis,
                                loop_storage *loop) {
  size_t block_index;
  for (block_index = 0u; block_index < loop->view.block_count; ++block_index) {
    const ql_ir_block_view_v1 *block =
        &analysis->blocks[loop->view.blocks[block_index]];
    size_t index;
    for (index = 0u; index < block->instruction_count; ++index) {
      const ql_ir_instruction_view_v1 *instruction =
          &analysis->instructions[block->instructions[index]];
      size_t operand;
      loop->view.effects |= instruction->effects;
      for (operand = 0u; operand < instruction->operand_count; ++operand) {
        note_value_state(analysis, loop, instruction->operands[operand]);
      }
      for (operand = 0u; operand < instruction->result_count; ++operand) {
        note_value_state(analysis, loop, instruction->results[operand]);
      }
    }
    note_value_state(analysis, loop, block->terminator.condition);
    note_value_state(analysis, loop, block->terminator.return_value);
    note_value_state(analysis, loop, block->terminator.memory);
    note_value_state(analysis, loop, block->terminator.event_trace);
  }
}

static int set_is_strict_subset(const loop_storage *left,
                                const loop_storage *right,
                                size_t block_count) {
  size_t index;
  int strictly_smaller = 0;
  for (index = 0u; index < block_count; ++index) {
    if (left->membership[index] != 0u && right->membership[index] == 0u) {
      return 0;
    }
    if (left->membership[index] == 0u && right->membership[index] != 0u) {
      strictly_smaller = 1;
    }
  }
  return strictly_smaller;
}

static int sets_overlap(const loop_storage *left, const loop_storage *right,
                        size_t block_count) {
  size_t index;
  for (index = 0u; index < block_count; ++index) {
    if (left->membership[index] != 0u && right->membership[index] != 0u) {
      return 1;
    }
  }
  return 0;
}

static void classify_loop_nesting(ql_loop_analysis *analysis) {
  size_t block_count = analysis->ir_view.block_count;
  size_t left;
  for (left = 0u; left < analysis->view.loop_count; ++left) {
    size_t right;
    size_t best = QL_LOOP_INVALID_INDEX;
    size_t best_size = SIZE_MAX;
    analysis->loops[left].view.parent = QL_LOOP_INVALID_INDEX;
    for (right = 0u; right < analysis->view.loop_count; ++right) {
      if (left == right) {
        continue;
      }
      if (set_is_strict_subset(&analysis->loops[left], &analysis->loops[right],
                               block_count) &&
          analysis->loops[right].view.block_count < best_size) {
        best = right;
        best_size = analysis->loops[right].view.block_count;
      }
    }
    analysis->loops[left].view.parent = best;
  }
  for (left = 0u; left < analysis->view.loop_count; ++left) {
    size_t parent = analysis->loops[left].view.parent;
    size_t depth = 0u;
    size_t guard = 0u;
    while (parent != QL_LOOP_INVALID_INDEX &&
           guard++ < analysis->view.loop_count) {
      ++depth;
      parent = analysis->loops[parent].view.parent;
    }
    analysis->loops[left].view.nesting_depth = depth;
  }
  for (left = 0u; left < analysis->view.loop_count; ++left) {
    size_t right;
    for (right = left + 1u; right < analysis->view.loop_count; ++right) {
      if (!sets_overlap(&analysis->loops[left], &analysis->loops[right],
                        block_count) ||
          set_is_strict_subset(&analysis->loops[left], &analysis->loops[right],
                               block_count) ||
          set_is_strict_subset(&analysis->loops[right], &analysis->loops[left],
                               block_count)) {
        continue;
      }
      analysis->loops[left].view.is_reducible = 0u;
      analysis->loops[left].view.is_reducible_single_entry = 0u;
      analysis->loops[right].view.is_reducible = 0u;
      analysis->loops[right].view.is_reducible_single_entry = 0u;
    }
  }
}

static ql_status build_loops(ql_loop_analysis *analysis, ql_error *error) {
  size_t block_count = analysis->ir_view.block_count;
  uint8_t *is_header = NULL;
  size_t loop_count = 0u;
  size_t index;
  size_t loop_index = 0u;
  ql_status status = allocate_zeroed(&analysis->allocator, block_count,
                                     sizeof(*is_header),
                                     (void **)&is_header, error);

  if (status != QL_STATUS_OK) {
    return status;
  }
  for (index = 0u; index < analysis->view.edge_count; ++index) {
    if (analysis->edge_is_backedge[index] != 0u &&
        is_header[analysis->edges[index].to] == 0u) {
      is_header[analysis->edges[index].to] = 1u;
      ++loop_count;
    }
  }
  status = allocate_zeroed(&analysis->allocator, loop_count,
                           sizeof(*analysis->loops),
                           (void **)&analysis->loops, error);
  if (status != QL_STATUS_OK) {
    analysis->allocator.deallocate(analysis->allocator.user_data, is_header);
    return status;
  }
  analysis->view.loop_count = loop_count;
  for (index = 0u; index < block_count; ++index) {
    loop_storage *loop;
    if (is_header[index] == 0u) {
      continue;
    }
    loop = &analysis->loops[loop_index];
    loop->view.index = loop_index;
    loop->view.header = (ql_ir_block_id)index;
    loop->view.preheader = QL_IR_INVALID_BLOCK_ID;
    loop->view.parent = QL_LOOP_INVALID_INDEX;
    status = build_loop_membership(analysis, loop, error);
    if (status == QL_STATUS_OK) {
      status = classify_loop_entry_and_exits(analysis, loop, error);
    }
    if (status == QL_STATUS_OK) {
      classify_guard(analysis, loop);
      status = build_loop_phis(analysis, loop, error);
    }
    if (status == QL_STATUS_OK) {
      classify_loop_state(analysis, loop);
    }
    if (status != QL_STATUS_OK) {
      break;
    }
    ++loop_index;
  }
  analysis->allocator.deallocate(analysis->allocator.user_data, is_header);
  if (status == QL_STATUS_OK) {
    classify_loop_nesting(analysis);
  }
  return status;
}

ql_status ql_loop_analysis_create(const ql_allocator *allocator,
                                  const ql_ir *ir,
                                  ql_loop_analysis **output,
                                  ql_error *error) {
  const ql_allocator *selected = resolve_allocator(allocator);
  ql_loop_analysis *analysis;
  ql_status status;

  if (ir == NULL || output == NULL) {
    ql_error_set(error, QL_STATUS_INVALID_ARGUMENT,
                 "IR and loop-analysis output are required");
    return QL_STATUS_INVALID_ARGUMENT;
  }
  *output = NULL;
  analysis = selected->allocate(selected->user_data, sizeof(*analysis));
  if (analysis == NULL) {
    ql_error_set(error, QL_STATUS_OUT_OF_MEMORY, NULL);
    return QL_STATUS_OUT_OF_MEMORY;
  }
  memset(analysis, 0, sizeof(*analysis));
  analysis->allocator = *selected;
  analysis->ir = ir;
  ql_ir_retain((ql_ir *)ir);
  status = cache_ir_views(analysis, error);
  if (status == QL_STATUS_OK) {
    status = build_cfg(analysis, error);
  }
  if (status == QL_STATUS_OK) {
    status = build_reverse_postorder(analysis, error);
  }
  if (status == QL_STATUS_OK) {
    status = build_dominators(analysis, error);
  }
  if (status == QL_STATUS_OK) {
    status = classify_graph_cycles(analysis, error);
  }
  if (status == QL_STATUS_OK) {
    status = build_loops(analysis, error);
  }
  if (status != QL_STATUS_OK) {
    ql_loop_analysis_destroy(analysis);
    return status;
  }
  *output = analysis;
  ql_error_clear(error);
  return QL_STATUS_OK;
}

void ql_loop_analysis_destroy(ql_loop_analysis *analysis) {
  size_t loop_index;
  if (analysis == NULL) {
    return;
  }
  for (loop_index = 0u; loop_index < analysis->view.loop_count; ++loop_index) {
    loop_storage *loop = &analysis->loops[loop_index];
    size_t phi_index;
    for (phi_index = 0u; phi_index < loop->view.phi_count; ++phi_index) {
      analysis->allocator.deallocate(
          analysis->allocator.user_data,
          (void *)loop->view.phis[phi_index].latch_values);
    }
    analysis->allocator.deallocate(analysis->allocator.user_data,
                                   (void *)loop->view.phis);
    analysis->allocator.deallocate(analysis->allocator.user_data,
                                   (void *)loop->view.exits);
    analysis->allocator.deallocate(analysis->allocator.user_data,
                                   (void *)loop->view.latches);
    analysis->allocator.deallocate(analysis->allocator.user_data,
                                   (void *)loop->view.blocks);
    analysis->allocator.deallocate(analysis->allocator.user_data,
                                   loop->membership);
  }
  analysis->allocator.deallocate(analysis->allocator.user_data, analysis->loops);
  analysis->allocator.deallocate(analysis->allocator.user_data,
                                 analysis->dominators);
  analysis->allocator.deallocate(analysis->allocator.user_data, analysis->order);
  analysis->allocator.deallocate(analysis->allocator.user_data,
                                 analysis->predecessors);
  analysis->allocator.deallocate(analysis->allocator.user_data,
                                 analysis->predecessor_offsets);
  analysis->allocator.deallocate(analysis->allocator.user_data,
                                 analysis->edge_is_backedge);
  analysis->allocator.deallocate(analysis->allocator.user_data, analysis->edges);
  analysis->allocator.deallocate(analysis->allocator.user_data,
                                 analysis->successors);
  analysis->allocator.deallocate(analysis->allocator.user_data,
                                 analysis->successor_counts);
  analysis->allocator.deallocate(analysis->allocator.user_data, analysis->blocks);
  analysis->allocator.deallocate(analysis->allocator.user_data,
                                 analysis->instructions);
  analysis->allocator.deallocate(analysis->allocator.user_data, analysis->values);
  analysis->allocator.deallocate(analysis->allocator.user_data, analysis->types);
  ql_ir_release((ql_ir *)analysis->ir);
  analysis->allocator.deallocate(analysis->allocator.user_data, analysis);
}

const ql_loop_analysis_view *
ql_loop_analysis_get_view(const ql_loop_analysis *analysis) {
  return analysis != NULL ? &analysis->view : NULL;
}

const ql_loop_view *ql_loop_analysis_loop_at(const ql_loop_analysis *analysis,
                                             size_t index) {
  if (analysis == NULL || index >= analysis->view.loop_count) {
    return NULL;
  }
  return &analysis->loops[index].view;
}

uint32_t ql_loop_analysis_contains_block(const ql_loop_analysis *analysis,
                                         size_t loop_index,
                                         ql_ir_block_id block) {
  if (analysis == NULL || loop_index >= analysis->view.loop_count ||
      block >= analysis->ir_view.block_count) {
    return 0u;
  }
  return analysis->loops[loop_index].membership[block] != 0u;
}
