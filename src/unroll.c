#include "unroll.h"

#include <stdio.h>
#include <string.h>

/* The unrolled graph is indexed by (block, copy). `bound` retreating-edge
   traversals need `bound + 1` copies: the last copy exists so the final
   permitted traversal still lands in real blocks, and only the traversal
   after it reaches the cut. */

typedef struct unroll_state {
    ql_allocator allocator;
    const ql_ir *ir;
    ql_ir_view_v1 view;
    uint32_t bound;
    uint32_t copies;

    /* Borrowed per-block views, valid while `ir` is retained by the caller. */
    ql_ir_block_view_v1 *blocks;
    /* Reverse post order and each block's position in it. */
    ql_ir_block_id *order;
    uint32_t *order_index;
    /* One flag per (block, copy) instance, then the new block id per emitted
       instance. */
    uint8_t *instance_reachable;
    ql_ir_block_id *instance_block;
    /* The latest emitted instance of every original value. Parameters and
       constants are filled before emission; instruction results are filled as
       their defining copies are emitted. */
    ql_ir_value_id *latest;

    ql_ir_builder *builder;
    ql_ir_block_id cut_block;
    ql_ir_value_id false_constant;
    uint32_t cut_used;
    uint32_t retreating_edges;
    uint64_t blocks_emitted;
} unroll_state;

static void *state_allocate(unroll_state *state, size_t size) {
    void *memory = state->allocator.allocate(state->allocator.user_data, size);
    if (memory != NULL) {
        memset(memory, 0, size);
    }
    return memory;
}

static void state_dispose(unroll_state *state) {
    ql_allocator *allocator = &state->allocator;
    ql_ir_builder_destroy(state->builder);
    allocator->deallocate(allocator->user_data, state->blocks);
    allocator->deallocate(allocator->user_data, state->order);
    allocator->deallocate(allocator->user_data, state->order_index);
    allocator->deallocate(allocator->user_data, state->instance_reachable);
    allocator->deallocate(allocator->user_data, state->instance_block);
    allocator->deallocate(allocator->user_data, state->latest);
}

static size_t successor_list(const ql_ir_block_view_v1 *block,
                             ql_ir_block_id successors[2]) {
    switch (block->terminator.kind) {
    case QL_IR_TERMINATOR_BRANCH:
        successors[0] = block->terminator.target;
        return 1u;
    case QL_IR_TERMINATOR_COND_BRANCH:
        successors[0] = block->terminator.target;
        successors[1] = block->terminator.false_target;
        return 2u;
    default:
        return 0u;
    }
}

/* Iterative depth-first search producing reverse post order. Every cycle in
   any directed graph contains at least one edge that does not advance this
   order, so routing exactly those edges across copies leaves each copy
   acyclic and the whole unrolled graph acyclic. */
static ql_status compute_order(unroll_state *state, ql_error *error) {
    const size_t block_count = state->view.block_count;
    ql_ir_block_id *stack;
    size_t *child_cursor;
    uint8_t *visited;
    size_t depth = 0u;
    size_t emitted = block_count;
    ql_status status = QL_STATUS_OK;

    stack = state_allocate(state, block_count * sizeof(*stack));
    child_cursor = state_allocate(state, block_count * sizeof(*child_cursor));
    visited = state_allocate(state, block_count * sizeof(*visited));
    if (stack == NULL || child_cursor == NULL || visited == NULL) {
        ql_error_set(error, QL_STATUS_OUT_OF_MEMORY, NULL);
        status = QL_STATUS_OUT_OF_MEMORY;
        goto cleanup;
    }

    stack[depth] = state->view.entry_block;
    child_cursor[depth] = 0u;
    visited[state->view.entry_block] = 1u;
    ++depth;
    while (depth > 0u) {
        const ql_ir_block_id block = stack[depth - 1u];
        ql_ir_block_id successors[2];
        const size_t successor_count =
            successor_list(&state->blocks[block], successors);
        if (child_cursor[depth - 1u] < successor_count) {
            const ql_ir_block_id next =
                successors[child_cursor[depth - 1u]++];
            if (!visited[next]) {
                visited[next] = 1u;
                stack[depth] = next;
                child_cursor[depth] = 0u;
                ++depth;
            }
            continue;
        }
        --depth;
        /* Post order reversed: the last block to finish is first. */
        state->order[--emitted] = block;
    }
    /* ql_ir_open already rejected unreachable blocks, so the walk covered
       everything and `emitted` returned to zero. */
    if (emitted != 0u) {
        ql_error_set(error, QL_STATUS_INTERNAL_ERROR,
                     "the reverse post order missed a block the IR reader "
                     "accepted");
        status = QL_STATUS_INTERNAL_ERROR;
        goto cleanup;
    }
    {
        size_t index;
        for (index = 0u; index < block_count; ++index) {
            state->order_index[state->order[index]] = (uint32_t)index;
        }
    }

cleanup:
    state->allocator.deallocate(state->allocator.user_data, stack);
    state->allocator.deallocate(state->allocator.user_data, child_cursor);
    state->allocator.deallocate(state->allocator.user_data, visited);
    return status;
}

static int edge_retreats(const unroll_state *state, ql_ir_block_id from,
                         ql_ir_block_id to) {
    return state->order_index[to] <= state->order_index[from];
}

static size_t instance_index(const unroll_state *state, ql_ir_block_id block,
                             uint32_t copy) {
    return (size_t)copy * state->view.block_count + block;
}

/* Marks every (block, copy) instance an execution within the bound can
   reach, and records whether any final-copy retreating edge exists, which is
   what makes the cut block and its ASSUME(false) necessary at all. */
static ql_status compute_reachable(unroll_state *state, ql_error *error) {
    const size_t block_count = state->view.block_count;
    const size_t instance_count = (size_t)state->copies * block_count;
    size_t *queue;
    size_t head = 0u;
    size_t tail = 0u;
    ql_status status = QL_STATUS_OK;

    queue = state_allocate(state, instance_count * sizeof(*queue));
    if (queue == NULL) {
        ql_error_set(error, QL_STATUS_OUT_OF_MEMORY, NULL);
        return QL_STATUS_OUT_OF_MEMORY;
    }
    {
        const size_t entry = instance_index(state, state->view.entry_block, 0u);
        state->instance_reachable[entry] = 1u;
        queue[tail++] = entry;
    }
    while (head < tail) {
        const size_t current = queue[head++];
        const ql_ir_block_id block =
            (ql_ir_block_id)(current % block_count);
        const uint32_t copy = (uint32_t)(current / block_count);
        ql_ir_block_id successors[2];
        const size_t successor_count =
            successor_list(&state->blocks[block], successors);
        size_t index;
        for (index = 0u; index < successor_count; ++index) {
            uint32_t next_copy = copy;
            size_t next;
            if (edge_retreats(state, block, successors[index])) {
                if (copy == state->bound) {
                    state->cut_used = 1u;
                    continue;
                }
                next_copy = copy + 1u;
            }
            next = instance_index(state, successors[index], next_copy);
            if (!state->instance_reachable[next]) {
                state->instance_reachable[next] = 1u;
                queue[tail++] = next;
            }
        }
    }
    state->allocator.deallocate(state->allocator.user_data, queue);
    return status;
}

static ql_status copy_types(unroll_state *state, ql_error *error) {
    size_t index;
    for (index = 0u; index < state->view.type_count; ++index) {
        ql_ir_type_view_v1 type_view;
        ql_ir_type_definition_v1 definition;
        ql_ir_type_id created;
        ql_status status;

        memset(&type_view, 0, sizeof(type_view));
        type_view.struct_size = sizeof(type_view);
        status = ql_ir_type_at(state->ir, index, &type_view, error);
        if (status != QL_STATUS_OK) {
            return status;
        }
        ql_ir_type_definition_init(&definition, type_view.kind);
        definition.float_format = type_view.float_format;
        definition.bit_width = type_view.bit_width;
        definition.address_space = type_view.address_space;
        definition.element_type = type_view.element_type;
        definition.element_count = type_view.element_count;
        status = ql_ir_builder_add_type(state->builder, &definition, &created,
                                        error);
        if (status != QL_STATUS_OK) {
            return status;
        }
        if (created != type_view.id) {
            ql_error_set(error, QL_STATUS_INTERNAL_ERROR,
                         "the rebuilt type table renumbered a type");
            return QL_STATUS_INTERNAL_ERROR;
        }
    }
    return QL_STATUS_OK;
}

/* Parameters and constants are re-added in original table order so the
   parameter ordinals a bound signature refers to stay what they were. */
static ql_status copy_parameters_and_constants(unroll_state *state,
                                               ql_error *error) {
    size_t index;
    for (index = 0u; index < state->view.value_count; ++index) {
        ql_ir_value_view_v1 value_view;
        ql_ir_value_id created;
        ql_status status;

        memset(&value_view, 0, sizeof(value_view));
        value_view.struct_size = sizeof(value_view);
        status = ql_ir_value_at(state->ir, index, &value_view, error);
        if (status != QL_STATUS_OK) {
            return status;
        }
        switch (value_view.definition_kind) {
        case QL_IR_VALUE_PARAMETER:
            status = ql_ir_builder_add_parameter(
                state->builder, value_view.type, value_view.name,
                value_view.name_size, &created, error);
            break;
        case QL_IR_VALUE_CONSTANT:
            status = ql_ir_builder_add_constant(
                state->builder, value_view.type, value_view.constant_data,
                value_view.constant_size, &created, error);
            break;
        default:
            state->latest[value_view.id] = QL_IR_INVALID_VALUE_ID;
            continue;
        }
        if (status != QL_STATUS_OK) {
            return status;
        }
        state->latest[value_view.id] = created;
    }
    return QL_STATUS_OK;
}

static ql_status create_blocks(unroll_state *state, ql_error *error) {
    const size_t block_count = state->view.block_count;
    uint32_t copy;
    ql_status status;

    for (copy = 0u; copy < state->copies; ++copy) {
        size_t position;
        for (position = 0u; position < block_count; ++position) {
            const ql_ir_block_id block = state->order[position];
            const size_t instance = instance_index(state, block, copy);
            char label[64];
            int written;
            if (!state->instance_reachable[instance]) {
                continue;
            }
            written = snprintf(label, sizeof(label), "u%u.%.*s", copy,
                               (int)(state->blocks[block].label_size < 48u
                                         ? state->blocks[block].label_size
                                         : 48u),
                               state->blocks[block].label != NULL
                                   ? state->blocks[block].label
                                   : "b");
            if (written < 0) {
                ql_error_set(error, QL_STATUS_INTERNAL_ERROR,
                             "could not format an unrolled block label");
                return QL_STATUS_INTERNAL_ERROR;
            }
            status = ql_ir_builder_add_block(
                state->builder, label, (size_t)written,
                &state->instance_block[instance], error);
            if (status != QL_STATUS_OK) {
                return status;
            }
            ++state->blocks_emitted;
        }
    }
    if (state->cut_used) {
        static const char cut_label[] = "unroll.cut";
        status = ql_ir_builder_add_block(state->builder, cut_label,
                                         sizeof(cut_label) - 1u,
                                         &state->cut_block, error);
        if (status != QL_STATUS_OK) {
            return status;
        }
        ++state->blocks_emitted;
    }
    return QL_STATUS_OK;
}

/* The cut block needs a false Boolean to assume. The constant is added only
   when a cut exists, so a graph that never reaches the bound gets a byte-for-
   byte reproduction of its value table. */
static ql_status add_false_constant(unroll_state *state, ql_error *error) {
    static const uint8_t false_byte = 0u;
    ql_ir_type_id bool_type = QL_IR_INVALID_TYPE_ID;
    size_t index;
    ql_status status;

    for (index = 0u; index < state->view.type_count; ++index) {
        ql_ir_type_view_v1 type_view;
        memset(&type_view, 0, sizeof(type_view));
        type_view.struct_size = sizeof(type_view);
        status = ql_ir_type_at(state->ir, index, &type_view, error);
        if (status != QL_STATUS_OK) {
            return status;
        }
        if (type_view.kind == QL_IR_TYPE_BOOL) {
            bool_type = type_view.id;
            break;
        }
    }
    if (bool_type == QL_IR_INVALID_TYPE_ID) {
        ql_ir_type_definition_v1 definition;
        ql_ir_type_definition_init(&definition, QL_IR_TYPE_BOOL);
        status = ql_ir_builder_add_type(state->builder, &definition,
                                        &bool_type, error);
        if (status != QL_STATUS_OK) {
            return status;
        }
    }
    return ql_ir_builder_add_constant(state->builder, bool_type, &false_byte,
                                      1u, &state->false_constant, error);
}

static ql_status map_operand(const unroll_state *state, ql_ir_value_id value,
                             ql_ir_value_id *mapped, ql_error *error) {
    if (value == QL_IR_INVALID_VALUE_ID) {
        *mapped = QL_IR_INVALID_VALUE_ID;
        return QL_STATUS_OK;
    }
    if (value >= state->view.value_count ||
        state->latest[value] == QL_IR_INVALID_VALUE_ID) {
        ql_error_set(error, QL_STATUS_TYPE_MISMATCH,
                     "bounded unrolling cannot order a value use after any "
                     "emitted definition; the loop shape is unsupported");
        return QL_STATUS_TYPE_MISMATCH;
    }
    *mapped = state->latest[value];
    return QL_STATUS_OK;
}

static ql_status map_edge_target(const unroll_state *state,
                                 ql_ir_block_id from, uint32_t copy,
                                 ql_ir_block_id to, ql_ir_block_id *mapped,
                                 ql_error *error) {
    uint32_t target_copy = copy;
    size_t instance;

    if (edge_retreats(state, from, to)) {
        if (copy == state->bound) {
            *mapped = state->cut_block;
            return QL_STATUS_OK;
        }
        target_copy = copy + 1u;
    }
    instance = instance_index(state, to, target_copy);
    if (!state->instance_reachable[instance]) {
        ql_error_set(error, QL_STATUS_INTERNAL_ERROR,
                     "an emitted edge targets an instance the reachability "
                     "pass rejected");
        return QL_STATUS_INTERNAL_ERROR;
    }
    *mapped = state->instance_block[instance];
    return QL_STATUS_OK;
}

static ql_status emit_phi(unroll_state *state, ql_ir_block_id block,
                          uint32_t copy,
                          const ql_ir_instruction_view_v1 *instruction,
                          ql_ir_block_id emitted_block, ql_error *error) {
    ql_ir_value_id operands[QL_UNROLL_MAX_BLOCKS < 256u ? 256u : 256u];
    ql_ir_block_id block_operands[256u];
    ql_ir_type_id result_type;
    ql_ir_value_view_v1 result_view;
    ql_ir_instruction_definition_v1 definition;
    ql_ir_instruction_id emitted;
    ql_ir_value_id result;
    size_t kept = 0u;
    size_t index;
    ql_status status;

    if (instruction->operand_count > 256u) {
        ql_error_set(error, QL_STATUS_TYPE_MISMATCH,
                     "a PHI with more than 256 incomings is outside bounded "
                     "unrolling");
        return QL_STATUS_TYPE_MISMATCH;
    }
    for (index = 0u; index < instruction->operand_count; ++index) {
        const ql_ir_block_id pred = instruction->block_operands[index];
        size_t pred_instance;
        if (edge_retreats(state, pred, block)) {
            if (copy == 0u) {
                continue;
            }
            pred_instance = instance_index(state, pred, copy - 1u);
        } else {
            pred_instance = instance_index(state, pred, copy);
        }
        if (!state->instance_reachable[pred_instance]) {
            continue;
        }
        status = map_operand(state, instruction->operands[index],
                             &operands[kept], error);
        if (status != QL_STATUS_OK) {
            return status;
        }
        block_operands[kept] = state->instance_block[pred_instance];
        ++kept;
    }
    if (kept == 0u) {
        /* A reachable block always has an emitted predecessor except the
           entry, and the entry of a well-formed function carries no PHI. */
        ql_error_set(error, QL_STATUS_TYPE_MISMATCH,
                     "a PHI kept no incoming edge under bounded unrolling; "
                     "the loop shape is unsupported");
        return QL_STATUS_TYPE_MISMATCH;
    }

    memset(&result_view, 0, sizeof(result_view));
    result_view.struct_size = sizeof(result_view);
    status = ql_ir_value_at(state->ir, instruction->results[0], &result_view,
                            error);
    if (status != QL_STATUS_OK) {
        return status;
    }
    result_type = result_view.type;
    ql_ir_instruction_definition_init(&definition, QL_IR_OPCODE_PHI);
    definition.flags = instruction->flags;
    definition.effects = instruction->effects;
    definition.operands = operands;
    definition.operand_count = kept;
    definition.block_operands = block_operands;
    definition.block_operand_count = kept;
    definition.result_types = &result_type;
    definition.result_count = 1u;
    status = ql_ir_builder_append_instruction(state->builder, emitted_block,
                                              &definition, &emitted, &result,
                                              error);
    if (status != QL_STATUS_OK) {
        return status;
    }
    state->latest[instruction->results[0]] = result;
    return QL_STATUS_OK;
}

static ql_status emit_instruction(unroll_state *state,
                                  const ql_ir_instruction_view_v1 *instruction,
                                  ql_ir_block_id emitted_block,
                                  ql_error *error) {
    ql_ir_value_id operands[64];
    ql_ir_type_id result_types[8];
    ql_ir_value_id results[8];
    ql_ir_instruction_definition_v1 definition;
    ql_ir_instruction_id emitted;
    size_t index;
    ql_status status;

    if (instruction->operand_count > 64u || instruction->result_count > 8u) {
        ql_error_set(error, QL_STATUS_TYPE_MISMATCH,
                     "an instruction arity is outside bounded unrolling");
        return QL_STATUS_TYPE_MISMATCH;
    }
    if (instruction->block_operand_count != 0u) {
        ql_error_set(error, QL_STATUS_TYPE_MISMATCH,
                     "a non-PHI instruction with block operands is outside "
                     "bounded unrolling");
        return QL_STATUS_TYPE_MISMATCH;
    }
    for (index = 0u; index < instruction->operand_count; ++index) {
        status = map_operand(state, instruction->operands[index],
                             &operands[index], error);
        if (status != QL_STATUS_OK) {
            return status;
        }
    }
    for (index = 0u; index < instruction->result_count; ++index) {
        ql_ir_value_view_v1 result_view;
        memset(&result_view, 0, sizeof(result_view));
        result_view.struct_size = sizeof(result_view);
        status = ql_ir_value_at(state->ir, instruction->results[index],
                                &result_view, error);
        if (status != QL_STATUS_OK) {
            return status;
        }
        result_types[index] = result_view.type;
    }
    ql_ir_instruction_definition_init(&definition, instruction->opcode);
    definition.flags = instruction->flags;
    definition.effects = instruction->effects;
    definition.operands = operands;
    definition.operand_count = instruction->operand_count;
    definition.result_types = result_types;
    definition.result_count = instruction->result_count;
    definition.immediate = instruction->immediate;
    definition.symbol = instruction->symbol;
    definition.symbol_size = instruction->symbol_size;
    definition.image = instruction->image;
    definition.image_size = instruction->image_size;
    status = ql_ir_builder_append_instruction(state->builder, emitted_block,
                                              &definition, &emitted, results,
                                              error);
    if (status != QL_STATUS_OK) {
        return status;
    }
    for (index = 0u; index < instruction->result_count; ++index) {
        state->latest[instruction->results[index]] = results[index];
    }
    return QL_STATUS_OK;
}

static ql_status emit_terminator(unroll_state *state, ql_ir_block_id block,
                                 uint32_t copy, ql_ir_block_id emitted_block,
                                 ql_error *error) {
    const ql_ir_terminator_definition_v1 *original =
        &state->blocks[block].terminator;
    ql_ir_terminator_definition_v1 definition;
    ql_status status;

    ql_ir_terminator_definition_init(&definition, original->kind);
    definition.code = original->code;
    definition.reason = original->reason;
    definition.reason_size = original->reason_size;
    status = map_operand(state, original->condition, &definition.condition,
                         error);
    if (status == QL_STATUS_OK) {
        status = map_operand(state, original->return_value,
                             &definition.return_value, error);
    }
    if (status == QL_STATUS_OK) {
        status = map_operand(state, original->memory, &definition.memory,
                             error);
    }
    if (status == QL_STATUS_OK) {
        status = map_operand(state, original->event_trace,
                             &definition.event_trace, error);
    }
    if (status != QL_STATUS_OK) {
        return status;
    }
    if (original->kind == QL_IR_TERMINATOR_BRANCH ||
        original->kind == QL_IR_TERMINATOR_COND_BRANCH) {
        status = map_edge_target(state, block, copy, original->target,
                                 &definition.target, error);
        if (status != QL_STATUS_OK) {
            return status;
        }
    }
    if (original->kind == QL_IR_TERMINATOR_COND_BRANCH) {
        status = map_edge_target(state, block, copy, original->false_target,
                                 &definition.false_target, error);
        if (status != QL_STATUS_OK) {
            return status;
        }
    }
    return ql_ir_builder_set_terminator(state->builder, emitted_block,
                                        &definition, error);
}

static ql_status emit_cut_block(unroll_state *state, ql_error *error) {
    static const char reason[] = "the unroll bound was reached";
    ql_ir_instruction_definition_v1 assume;
    ql_ir_terminator_definition_v1 terminator;
    ql_ir_instruction_id emitted;
    ql_status status;

    ql_ir_instruction_definition_init(&assume, QL_IR_OPCODE_ASSUME);
    assume.operands = &state->false_constant;
    assume.operand_count = 1u;
    status = ql_ir_builder_append_instruction(state->builder, state->cut_block,
                                              &assume, &emitted, NULL, error);
    if (status != QL_STATUS_OK) {
        return status;
    }
    /* ASSUME(false) already removes the path from the comparison domain in
       both the encoding and the interpreter; the terminator only has to
       claim nothing, and DIVERGE claims no return value, no trap code, and
       no successor. */
    ql_ir_terminator_definition_init(&terminator, QL_IR_TERMINATOR_DIVERGE);
    terminator.reason = reason;
    terminator.reason_size = sizeof(reason) - 1u;
    return ql_ir_builder_set_terminator(state->builder, state->cut_block,
                                        &terminator, error);
}

static ql_status emit_bodies(unroll_state *state, ql_error *error) {
    const size_t block_count = state->view.block_count;
    uint32_t copy;
    ql_status status;

    for (copy = 0u; copy < state->copies; ++copy) {
        size_t position;
        for (position = 0u; position < block_count; ++position) {
            const ql_ir_block_id block = state->order[position];
            const size_t instance = instance_index(state, block, copy);
            size_t index;
            if (!state->instance_reachable[instance]) {
                continue;
            }
            for (index = 0u; index < state->blocks[block].instruction_count;
                 ++index) {
                ql_ir_instruction_view_v1 instruction;
                memset(&instruction, 0, sizeof(instruction));
                instruction.struct_size = sizeof(instruction);
                status = ql_ir_instruction_at(
                    state->ir, state->blocks[block].instructions[index],
                    &instruction, error);
                if (status != QL_STATUS_OK) {
                    return status;
                }
                if (instruction.opcode == QL_IR_OPCODE_PHI) {
                    status = emit_phi(state, block, copy, &instruction,
                                      state->instance_block[instance], error);
                } else {
                    status = emit_instruction(
                        state, &instruction, state->instance_block[instance],
                        error);
                }
                if (status != QL_STATUS_OK) {
                    return status;
                }
            }
            status = emit_terminator(state, block, copy,
                                     state->instance_block[instance], error);
            if (status != QL_STATUS_OK) {
                return status;
            }
        }
    }
    if (state->cut_used) {
        return emit_cut_block(state, error);
    }
    return QL_STATUS_OK;
}

ql_status ql_ir_unroll_bounded(const ql_allocator *allocator, const ql_ir *ir,
                               uint32_t bound, ql_unroll_stats_v1 *stats,
                               ql_artifact **output, ql_error *error) {
    unroll_state state;
    size_t block_index;
    size_t instance_count;
    ql_status status;

    if (ir == NULL || output == NULL) {
        ql_error_set(error, QL_STATUS_INVALID_ARGUMENT,
                     "an IR and an output are required");
        return QL_STATUS_INVALID_ARGUMENT;
    }
    *output = NULL;
    memset(&state, 0, sizeof(state));
    state.allocator =
        ql_allocator_is_valid(allocator) ? *allocator : *ql_default_allocator();
    state.ir = ir;
    state.bound = bound;
    state.copies = bound + 1u;
    state.cut_block = QL_IR_INVALID_BLOCK_ID;
    state.false_constant = QL_IR_INVALID_VALUE_ID;
    state.view.struct_size = sizeof(state.view);
    status = ql_ir_get_view(ir, &state.view, error);
    if (status != QL_STATUS_OK) {
        return status;
    }
    if (state.view.block_count == 0u || bound == 0u ||
        bound > QL_UNROLL_MAX_BLOCKS) {
        ql_error_set(error, QL_STATUS_INVALID_ARGUMENT,
                     "the unroll bound must be between 1 and %u",
                     (unsigned)QL_UNROLL_MAX_BLOCKS);
        return QL_STATUS_INVALID_ARGUMENT;
    }
    if ((size_t)state.copies * state.view.block_count >
        (size_t)QL_UNROLL_MAX_BLOCKS) {
        ql_error_set(error, QL_STATUS_TYPE_MISMATCH,
                     "unrolling %zu blocks %u times exceeds the %u block "
                     "instance limit",
                     state.view.block_count, (unsigned)state.copies,
                     (unsigned)QL_UNROLL_MAX_BLOCKS);
        return QL_STATUS_TYPE_MISMATCH;
    }
    instance_count = (size_t)state.copies * state.view.block_count;

    state.blocks =
        state_allocate(&state, state.view.block_count * sizeof(*state.blocks));
    state.order =
        state_allocate(&state, state.view.block_count * sizeof(*state.order));
    state.order_index = state_allocate(
        &state, state.view.block_count * sizeof(*state.order_index));
    state.instance_reachable =
        state_allocate(&state, instance_count * sizeof(uint8_t));
    state.instance_block = state_allocate(
        &state, instance_count * sizeof(*state.instance_block));
    state.latest =
        state_allocate(&state, state.view.value_count * sizeof(*state.latest));
    if (state.blocks == NULL || state.order == NULL ||
        state.order_index == NULL || state.instance_reachable == NULL ||
        state.instance_block == NULL ||
        (state.view.value_count != 0u && state.latest == NULL)) {
        ql_error_set(error, QL_STATUS_OUT_OF_MEMORY, NULL);
        status = QL_STATUS_OUT_OF_MEMORY;
        goto cleanup;
    }
    for (block_index = 0u; block_index < instance_count; ++block_index) {
        state.instance_block[block_index] = QL_IR_INVALID_BLOCK_ID;
    }
    for (block_index = 0u; block_index < state.view.value_count;
         ++block_index) {
        state.latest[block_index] = QL_IR_INVALID_VALUE_ID;
    }
    for (block_index = 0u; block_index < state.view.block_count;
         ++block_index) {
        state.blocks[block_index].struct_size = sizeof(state.blocks[0]);
        status = ql_ir_block_at(ir, block_index, &state.blocks[block_index],
                                error);
        if (status != QL_STATUS_OK) {
            goto cleanup;
        }
    }

    status = compute_order(&state, error);
    if (status != QL_STATUS_OK) {
        goto cleanup;
    }
    for (block_index = 0u; block_index < state.view.block_count;
         ++block_index) {
        ql_ir_block_id successors[2];
        const size_t successor_count =
            successor_list(&state.blocks[block_index], successors);
        size_t index;
        for (index = 0u; index < successor_count; ++index) {
            if (edge_retreats(&state, (ql_ir_block_id)block_index,
                              successors[index])) {
                ++state.retreating_edges;
            }
        }
    }
    status = compute_reachable(&state, error);
    if (status != QL_STATUS_OK) {
        goto cleanup;
    }

    status = ql_ir_builder_create(&state.allocator, &state.builder, error);
    if (status != QL_STATUS_OK) {
        goto cleanup;
    }
    status = copy_types(&state, error);
    if (status == QL_STATUS_OK) {
        status = ql_ir_builder_set_function(
            state.builder, state.view.function_name,
            state.view.function_name_size, state.view.return_type, error);
    }
    if (status == QL_STATUS_OK) {
        status = ql_ir_builder_set_cfg_kind(state.builder, QL_IR_CFG_ACYCLIC,
                                            error);
    }
    if (status == QL_STATUS_OK) {
        status = copy_parameters_and_constants(&state, error);
    }
    if (status == QL_STATUS_OK && state.cut_used) {
        status = add_false_constant(&state, error);
    }
    if (status == QL_STATUS_OK) {
        status = create_blocks(&state, error);
    }
    if (status == QL_STATUS_OK) {
        status = ql_ir_builder_set_entry_block(
            state.builder,
            state.instance_block[instance_index(&state, state.view.entry_block,
                                                0u)],
            error);
    }
    if (status == QL_STATUS_OK) {
        status = emit_bodies(&state, error);
    }
    if (status == QL_STATUS_OK) {
        status = ql_ir_builder_finish(state.builder, output, error);
    }
    if (status == QL_STATUS_OK && stats != NULL) {
        memset(stats, 0, sizeof(*stats));
        stats->struct_size = sizeof(*stats);
        stats->blocks_emitted = state.blocks_emitted;
        stats->bound_cut_used = state.cut_used;
        stats->retreating_edges = state.retreating_edges;
    }

cleanup:
    state_dispose(&state);
    return status;
}
