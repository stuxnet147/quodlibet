#include "quodlibet/aig.h"

#include <stdio.h>
#include <string.h>

/* Temporaries are sized by the width bound rather than allocated, so an
   operation cannot fail partway through for want of memory and leave a caller
   holding half a circuit. One extra bit is carried for restoring division. */
#define AIG_TEMP_WIDTH (QL_AIG_MAX_BIT_WIDTH + 1u)

typedef struct aig_node {
    /* Both zero for an input node. An AND node never has a constant operand,
       because ql_aig_and folds those away before a node is created. */
    ql_aig_lit left;
    ql_aig_lit right;
    uint32_t is_input;
    uint32_t input_index;
} aig_node;

struct ql_aig {
    ql_allocator allocator;
    aig_node *nodes;
    size_t node_count;
    size_t node_capacity;
    uint32_t *inputs;
    size_t input_count;
    size_t input_capacity;
    /* Open-addressed structural hash over AND nodes. A slot holds a node
       index; zero means empty, which is unambiguous because node zero is the
       constant and is never hashed. */
    uint32_t *table;
    size_t table_size;
    uint64_t node_limit;
    uint64_t shared_count;
    uint64_t folded_count;
};

static const ql_allocator *select_allocator(const ql_allocator *allocator) {
    return allocator == NULL ? ql_default_allocator() : allocator;
}

static ql_aig_lit node_to_literal(size_t node) {
    return (ql_aig_lit)(node << 1);
}

static size_t literal_to_node(ql_aig_lit literal) {
    return (size_t)(literal >> 1);
}

/* --- Literal helpers ------------------------------------------------------ */

void QL_CALL ql_aig_view_init(ql_aig_view_v1 *view) {
    if (view == NULL) {
        return;
    }
    memset(view, 0, sizeof(*view));
    view->struct_size = sizeof(*view);
    view->schema_version = QL_AIG_SCHEMA_VERSION;
}

ql_aig_lit QL_CALL ql_aig_not(ql_aig_lit literal) {
    return literal ^ UINT32_C(1);
}

uint32_t QL_CALL ql_aig_is_constant(ql_aig_lit literal) {
    return literal <= QL_AIG_LIT_TRUE ? 1u : 0u;
}

uint32_t QL_CALL ql_aig_is_inverted(ql_aig_lit literal) {
    return (literal & UINT32_C(1)) != 0u ? 1u : 0u;
}

uint32_t QL_CALL ql_aig_literal_is_valid(const ql_aig *aig,
                                         ql_aig_lit literal) {
    if (aig == NULL || literal == QL_AIG_LIT_INVALID) {
        return 0u;
    }
    return literal_to_node(literal) < aig->node_count ? 1u : 0u;
}

/* --- Storage -------------------------------------------------------------- */

static ql_status reserve_nodes(ql_aig *aig, ql_error *error) {
    size_t capacity;
    aig_node *grown;

    if (aig->node_count < aig->node_capacity) {
        return QL_STATUS_OK;
    }
    capacity = aig->node_capacity == 0u ? 64u : aig->node_capacity * 2u;
    grown = aig->allocator.reallocate(aig->allocator.user_data, aig->nodes,
                                      capacity * sizeof(*grown));
    if (grown == NULL) {
        ql_error_set(error, QL_STATUS_OUT_OF_MEMORY, NULL);
        return QL_STATUS_OUT_OF_MEMORY;
    }
    aig->nodes = grown;
    aig->node_capacity = capacity;
    return QL_STATUS_OK;
}

static size_t hash_pair(ql_aig_lit left, ql_aig_lit right) {
    uint64_t value = ((uint64_t)left << 32) | (uint64_t)right;

    value ^= value >> 33;
    value *= UINT64_C(0xff51afd7ed558ccd);
    value ^= value >> 33;
    value *= UINT64_C(0xc4ceb9fe1a85ec53);
    value ^= value >> 33;
    return (size_t)value;
}

static void table_insert(ql_aig *aig, uint32_t node) {
    const size_t mask = aig->table_size - 1u;
    size_t slot = hash_pair(aig->nodes[node].left, aig->nodes[node].right) &
                  mask;

    while (aig->table[slot] != 0u) {
        slot = (slot + 1u) & mask;
    }
    aig->table[slot] = node;
}

static ql_status table_grow(ql_aig *aig, ql_error *error) {
    const size_t capacity = aig->table_size == 0u ? 256u : aig->table_size * 2u;
    uint32_t *table = aig->allocator.allocate(aig->allocator.user_data,
                                              capacity * sizeof(*table));
    uint32_t *previous = aig->table;
    const size_t previous_size = aig->table_size;
    size_t index;

    if (table == NULL) {
        ql_error_set(error, QL_STATUS_OUT_OF_MEMORY, NULL);
        return QL_STATUS_OUT_OF_MEMORY;
    }
    memset(table, 0, capacity * sizeof(*table));
    aig->table = table;
    aig->table_size = capacity;
    for (index = 0u; index < previous_size; ++index) {
        if (previous[index] != 0u) {
            table_insert(aig, previous[index]);
        }
    }
    aig->allocator.deallocate(aig->allocator.user_data, previous);
    return QL_STATUS_OK;
}

static uint32_t table_lookup(const ql_aig *aig, ql_aig_lit left,
                             ql_aig_lit right) {
    size_t mask;
    size_t slot;

    if (aig->table_size == 0u) {
        return 0u;
    }
    mask = aig->table_size - 1u;
    slot = hash_pair(left, right) & mask;
    while (aig->table[slot] != 0u) {
        const aig_node *node = &aig->nodes[aig->table[slot]];
        if (node->left == left && node->right == right) {
            return aig->table[slot];
        }
        slot = (slot + 1u) & mask;
    }
    return 0u;
}

/* --- Lifetime ------------------------------------------------------------- */

ql_status QL_CALL ql_aig_create(const ql_allocator *allocator, ql_aig **output,
                                ql_error *error) {
    const ql_allocator *selected = select_allocator(allocator);
    ql_aig *aig;

    if (output == NULL) {
        ql_error_set(error, QL_STATUS_INVALID_ARGUMENT,
                     "an output pointer is required");
        return QL_STATUS_INVALID_ARGUMENT;
    }
    *output = NULL;
    if (!ql_allocator_is_valid(selected)) {
        ql_error_set(error, QL_STATUS_INVALID_ARGUMENT, "invalid allocator");
        return QL_STATUS_INVALID_ARGUMENT;
    }
    aig = selected->allocate(selected->user_data, sizeof(*aig));
    if (aig == NULL) {
        ql_error_set(error, QL_STATUS_OUT_OF_MEMORY, NULL);
        return QL_STATUS_OUT_OF_MEMORY;
    }
    memset(aig, 0, sizeof(*aig));
    aig->allocator = *selected;
    if (reserve_nodes(aig, error) != QL_STATUS_OK) {
        selected->deallocate(selected->user_data, aig);
        return QL_STATUS_OUT_OF_MEMORY;
    }
    /* Node zero is the constant false. Literal zero is therefore false and
       literal one is true, with no node allocated for either. */
    memset(&aig->nodes[0], 0, sizeof(aig->nodes[0]));
    aig->node_count = 1u;
    *output = aig;
    ql_error_clear(error);
    return QL_STATUS_OK;
}

void QL_CALL ql_aig_destroy(ql_aig *aig) {
    ql_allocator allocator;

    if (aig == NULL) {
        return;
    }
    allocator = aig->allocator;
    allocator.deallocate(allocator.user_data, aig->nodes);
    allocator.deallocate(allocator.user_data, aig->inputs);
    allocator.deallocate(allocator.user_data, aig->table);
    allocator.deallocate(allocator.user_data, aig);
}

void QL_CALL ql_aig_set_node_limit(ql_aig *aig, uint64_t limit) {
    if (aig != NULL) {
        aig->node_limit = limit;
    }
}

ql_status QL_CALL ql_aig_get_view(const ql_aig *aig, ql_aig_view_v1 *view,
                                  ql_error *error) {
    if (aig == NULL || view == NULL) {
        ql_error_set(error, QL_STATUS_INVALID_ARGUMENT,
                     "graph and view are required");
        return QL_STATUS_INVALID_ARGUMENT;
    }
    if (view->struct_size != 0u && view->struct_size < sizeof(*view)) {
        ql_error_set(error, QL_STATUS_ABI_MISMATCH,
                     "AIG view structure is too small");
        return QL_STATUS_ABI_MISMATCH;
    }
    ql_aig_view_init(view);
    view->and_count = (uint64_t)(aig->node_count - 1u - aig->input_count);
    view->input_count = (uint64_t)aig->input_count;
    view->shared_count = aig->shared_count;
    view->folded_count = aig->folded_count;
    ql_error_clear(error);
    return QL_STATUS_OK;
}

/* --- Inputs --------------------------------------------------------------- */

ql_status QL_CALL ql_aig_add_input(ql_aig *aig, ql_aig_lit *output,
                                   ql_error *error) {
    ql_status status;

    if (aig == NULL || output == NULL) {
        ql_error_set(error, QL_STATUS_INVALID_ARGUMENT,
                     "graph and output are required");
        return QL_STATUS_INVALID_ARGUMENT;
    }
    *output = QL_AIG_LIT_INVALID;
    if (aig->node_limit != 0u &&
        (uint64_t)aig->node_count >= aig->node_limit) {
        ql_error_set(error, QL_STATUS_METHOD_ERROR,
                     "the circuit exceeded its node limit of %llu",
                     (unsigned long long)aig->node_limit);
        return QL_STATUS_METHOD_ERROR;
    }
    if (aig->input_count == aig->input_capacity) {
        const size_t capacity =
            aig->input_capacity == 0u ? 32u : aig->input_capacity * 2u;
        uint32_t *grown = aig->allocator.reallocate(
            aig->allocator.user_data, aig->inputs, capacity * sizeof(*grown));
        if (grown == NULL) {
            ql_error_set(error, QL_STATUS_OUT_OF_MEMORY, NULL);
            return QL_STATUS_OUT_OF_MEMORY;
        }
        aig->inputs = grown;
        aig->input_capacity = capacity;
    }
    status = reserve_nodes(aig, error);
    if (status != QL_STATUS_OK) {
        return status;
    }
    aig->nodes[aig->node_count].left = QL_AIG_LIT_FALSE;
    aig->nodes[aig->node_count].right = QL_AIG_LIT_FALSE;
    aig->nodes[aig->node_count].is_input = 1u;
    aig->nodes[aig->node_count].input_index = (uint32_t)aig->input_count;
    aig->inputs[aig->input_count] = (uint32_t)aig->node_count;
    ++aig->input_count;
    *output = node_to_literal(aig->node_count);
    ++aig->node_count;
    ql_error_clear(error);
    return QL_STATUS_OK;
}

uint32_t QL_CALL ql_aig_input_index(const ql_aig *aig, ql_aig_lit literal) {
    size_t node;

    if (!ql_aig_literal_is_valid(aig, literal) ||
        ql_aig_is_inverted(literal) != 0u) {
        return QL_AIG_LIT_INVALID;
    }
    node = literal_to_node(literal);
    if (node == 0u || aig->nodes[node].is_input == 0u) {
        return QL_AIG_LIT_INVALID;
    }
    return aig->nodes[node].input_index;
}

ql_status QL_CALL ql_aig_input_literal(const ql_aig *aig, uint32_t index,
                                       ql_aig_lit *output, ql_error *error) {
    if (aig == NULL || output == NULL) {
        ql_error_set(error, QL_STATUS_INVALID_ARGUMENT,
                     "graph and output are required");
        return QL_STATUS_INVALID_ARGUMENT;
    }
    *output = QL_AIG_LIT_INVALID;
    if ((size_t)index >= aig->input_count) {
        ql_error_set(error, QL_STATUS_NOT_FOUND,
                     "no input %u exists in this graph", index);
        return QL_STATUS_NOT_FOUND;
    }
    *output = node_to_literal(aig->inputs[index]);
    ql_error_clear(error);
    return QL_STATUS_OK;
}

/* --- Construction --------------------------------------------------------- */

ql_status QL_CALL ql_aig_and(ql_aig *aig, ql_aig_lit left, ql_aig_lit right,
                             ql_aig_lit *output, ql_error *error) {
    uint32_t existing;
    ql_status status;

    if (aig == NULL || output == NULL) {
        ql_error_set(error, QL_STATUS_INVALID_ARGUMENT,
                     "graph and output are required");
        return QL_STATUS_INVALID_ARGUMENT;
    }
    *output = QL_AIG_LIT_INVALID;
    if (!ql_aig_literal_is_valid(aig, left) ||
        !ql_aig_literal_is_valid(aig, right)) {
        ql_error_set(error, QL_STATUS_INVALID_ARGUMENT,
                     "an operand literal does not belong to this graph");
        return QL_STATUS_INVALID_ARGUMENT;
    }
    /* The five one-step identities. Each one that fires is a node the solver
       never sees. */
    if (left == QL_AIG_LIT_FALSE || right == QL_AIG_LIT_FALSE ||
        left == ql_aig_not(right)) {
        ++aig->folded_count;
        *output = QL_AIG_LIT_FALSE;
        ql_error_clear(error);
        return QL_STATUS_OK;
    }
    if (left == QL_AIG_LIT_TRUE || left == right) {
        ++aig->folded_count;
        *output = right;
        ql_error_clear(error);
        return QL_STATUS_OK;
    }
    if (right == QL_AIG_LIT_TRUE) {
        ++aig->folded_count;
        *output = left;
        ql_error_clear(error);
        return QL_STATUS_OK;
    }
    if (left > right) {
        const ql_aig_lit swap = left;
        left = right;
        right = swap;
    }
    existing = table_lookup(aig, left, right);
    if (existing != 0u) {
        ++aig->shared_count;
        *output = node_to_literal(existing);
        ql_error_clear(error);
        return QL_STATUS_OK;
    }
    if (aig->node_limit != 0u &&
        (uint64_t)aig->node_count >= aig->node_limit) {
        ql_error_set(error, QL_STATUS_METHOD_ERROR,
                     "the circuit exceeded its node limit of %llu",
                     (unsigned long long)aig->node_limit);
        return QL_STATUS_METHOD_ERROR;
    }
    status = reserve_nodes(aig, error);
    if (status != QL_STATUS_OK) {
        return status;
    }
    if (aig->table_size == 0u || aig->node_count * 2u >= aig->table_size) {
        status = table_grow(aig, error);
        if (status != QL_STATUS_OK) {
            return status;
        }
    }
    aig->nodes[aig->node_count].left = left;
    aig->nodes[aig->node_count].right = right;
    aig->nodes[aig->node_count].is_input = 0u;
    aig->nodes[aig->node_count].input_index = 0u;
    table_insert(aig, (uint32_t)aig->node_count);
    *output = node_to_literal(aig->node_count);
    ++aig->node_count;
    ql_error_clear(error);
    return QL_STATUS_OK;
}

ql_status QL_CALL ql_aig_or(ql_aig *aig, ql_aig_lit left, ql_aig_lit right,
                            ql_aig_lit *output, ql_error *error) {
    ql_status status = ql_aig_and(aig, ql_aig_not(left), ql_aig_not(right),
                                  output, error);
    if (status == QL_STATUS_OK) {
        *output = ql_aig_not(*output);
    }
    return status;
}

ql_status QL_CALL ql_aig_xor(ql_aig *aig, ql_aig_lit left, ql_aig_lit right,
                             ql_aig_lit *output, ql_error *error) {
    ql_aig_lit both;
    ql_aig_lit neither;
    ql_status status;

    if (output == NULL) {
        ql_error_set(error, QL_STATUS_INVALID_ARGUMENT,
                     "an output pointer is required");
        return QL_STATUS_INVALID_ARGUMENT;
    }
    status = ql_aig_and(aig, left, right, &both, error);
    if (status != QL_STATUS_OK) {
        return status;
    }
    status = ql_aig_and(aig, ql_aig_not(left), ql_aig_not(right), &neither,
                        error);
    if (status != QL_STATUS_OK) {
        return status;
    }
    status = ql_aig_and(aig, ql_aig_not(both), ql_aig_not(neither), output,
                        error);
    return status;
}

ql_status QL_CALL ql_aig_xnor(ql_aig *aig, ql_aig_lit left, ql_aig_lit right,
                              ql_aig_lit *output, ql_error *error) {
    ql_status status = ql_aig_xor(aig, left, right, output, error);
    if (status == QL_STATUS_OK) {
        *output = ql_aig_not(*output);
    }
    return status;
}

ql_status QL_CALL ql_aig_implies(ql_aig *aig, ql_aig_lit antecedent,
                                 ql_aig_lit consequent, ql_aig_lit *output,
                                 ql_error *error) {
    return ql_aig_or(aig, ql_aig_not(antecedent), consequent, output, error);
}

ql_status QL_CALL ql_aig_mux(ql_aig *aig, ql_aig_lit condition,
                             ql_aig_lit when_true, ql_aig_lit when_false,
                             ql_aig_lit *output, ql_error *error) {
    ql_aig_lit taken;
    ql_aig_lit skipped;
    ql_status status;

    if (output == NULL) {
        ql_error_set(error, QL_STATUS_INVALID_ARGUMENT,
                     "an output pointer is required");
        return QL_STATUS_INVALID_ARGUMENT;
    }
    if (when_true == when_false) {
        *output = when_true;
        ql_error_clear(error);
        return QL_STATUS_OK;
    }
    status = ql_aig_and(aig, condition, when_true, &taken, error);
    if (status != QL_STATUS_OK) {
        return status;
    }
    status = ql_aig_and(aig, ql_aig_not(condition), when_false, &skipped,
                        error);
    if (status != QL_STATUS_OK) {
        return status;
    }
    return ql_aig_or(aig, taken, skipped, output, error);
}

/* --- Bit-vector helpers --------------------------------------------------- */

static ql_status check_width(uint32_t width, ql_error *error) {
    if (width == 0u || width > QL_AIG_MAX_BIT_WIDTH) {
        ql_error_set(error, QL_STATUS_INVALID_ARGUMENT,
                     "a bit-vector width of %u is outside 1..%u", width,
                     QL_AIG_MAX_BIT_WIDTH);
        return QL_STATUS_INVALID_ARGUMENT;
    }
    return QL_STATUS_OK;
}

ql_status QL_CALL ql_aig_bv_fill(ql_aig_lit value, uint32_t width,
                                 ql_aig_lit *output) {
    uint32_t index;

    if (output == NULL) {
        return QL_STATUS_INVALID_ARGUMENT;
    }
    for (index = 0u; index < width; ++index) {
        output[index] = value;
    }
    return QL_STATUS_OK;
}

ql_status QL_CALL ql_aig_bv_constant(ql_aig *aig, const void *bytes,
                                     size_t size, uint32_t width,
                                     ql_aig_lit *output, ql_error *error) {
    const uint8_t *data = (const uint8_t *)bytes;
    uint32_t index;
    ql_status status;

    (void)aig;
    if (output == NULL || bytes == NULL) {
        ql_error_set(error, QL_STATUS_INVALID_ARGUMENT,
                     "constant bytes and an output are required");
        return QL_STATUS_INVALID_ARGUMENT;
    }
    status = check_width(width, error);
    if (status != QL_STATUS_OK) {
        return status;
    }
    if (size != ((size_t)width + 7u) / 8u) {
        ql_error_set(error, QL_STATUS_INVALID_ARGUMENT,
                     "a %u-bit constant needs exactly %zu bytes, not %zu",
                     width, ((size_t)width + 7u) / 8u, size);
        return QL_STATUS_INVALID_ARGUMENT;
    }
    if ((width % 8u) != 0u) {
        const uint8_t mask = (uint8_t)((1u << (width % 8u)) - 1u);
        if ((data[size - 1u] & (uint8_t)~mask) != 0u) {
            ql_error_set(error, QL_STATUS_INVALID_ARGUMENT,
                         "the unused high bits of a %u-bit constant are not zero",
                         width);
            return QL_STATUS_INVALID_ARGUMENT;
        }
    }
    for (index = 0u; index < width; ++index) {
        const uint8_t bit =
            (uint8_t)((data[index / 8u] >> (index % 8u)) & 1u);
        output[index] = bit != 0u ? QL_AIG_LIT_TRUE : QL_AIG_LIT_FALSE;
    }
    ql_error_clear(error);
    return QL_STATUS_OK;
}

ql_status QL_CALL ql_aig_bv_input(ql_aig *aig, uint32_t width,
                                  ql_aig_lit *output, ql_error *error) {
    uint32_t index;
    ql_status status = check_width(width, error);

    if (output == NULL) {
        ql_error_set(error, QL_STATUS_INVALID_ARGUMENT,
                     "an output pointer is required");
        return QL_STATUS_INVALID_ARGUMENT;
    }
    if (status != QL_STATUS_OK) {
        return status;
    }
    for (index = 0u; index < width; ++index) {
        status = ql_aig_add_input(aig, &output[index], error);
        if (status != QL_STATUS_OK) {
            return status;
        }
    }
    return QL_STATUS_OK;
}

ql_status QL_CALL ql_aig_bv_not(ql_aig *aig, const ql_aig_lit *operand,
                                uint32_t width, ql_aig_lit *output,
                                ql_error *error) {
    uint32_t index;
    ql_status status = check_width(width, error);

    (void)aig;
    if (operand == NULL || output == NULL) {
        ql_error_set(error, QL_STATUS_INVALID_ARGUMENT,
                     "operand and output are required");
        return QL_STATUS_INVALID_ARGUMENT;
    }
    if (status != QL_STATUS_OK) {
        return status;
    }
    for (index = 0u; index < width; ++index) {
        output[index] = ql_aig_not(operand[index]);
    }
    return QL_STATUS_OK;
}

typedef ql_status (QL_CALL *bitwise_fn)(ql_aig *, ql_aig_lit, ql_aig_lit,
                                        ql_aig_lit *, ql_error *);

static ql_status bitwise(ql_aig *aig, bitwise_fn operation,
                         const ql_aig_lit *left, const ql_aig_lit *right,
                         uint32_t width, ql_aig_lit *output,
                         ql_error *error) {
    ql_aig_lit temporary[QL_AIG_MAX_BIT_WIDTH];
    uint32_t index;
    ql_status status = check_width(width, error);

    if (left == NULL || right == NULL || output == NULL) {
        ql_error_set(error, QL_STATUS_INVALID_ARGUMENT,
                     "both operands and an output are required");
        return QL_STATUS_INVALID_ARGUMENT;
    }
    if (status != QL_STATUS_OK) {
        return status;
    }
    for (index = 0u; index < width; ++index) {
        status = operation(aig, left[index], right[index], &temporary[index],
                           error);
        if (status != QL_STATUS_OK) {
            return status;
        }
    }
    memcpy(output, temporary, (size_t)width * sizeof(*output));
    return QL_STATUS_OK;
}

ql_status QL_CALL ql_aig_bv_and(ql_aig *aig, const ql_aig_lit *left,
                                const ql_aig_lit *right, uint32_t width,
                                ql_aig_lit *output, ql_error *error) {
    return bitwise(aig, ql_aig_and, left, right, width, output, error);
}

ql_status QL_CALL ql_aig_bv_or(ql_aig *aig, const ql_aig_lit *left,
                               const ql_aig_lit *right, uint32_t width,
                               ql_aig_lit *output, ql_error *error) {
    return bitwise(aig, ql_aig_or, left, right, width, output, error);
}

ql_status QL_CALL ql_aig_bv_xor(ql_aig *aig, const ql_aig_lit *left,
                                const ql_aig_lit *right, uint32_t width,
                                ql_aig_lit *output, ql_error *error) {
    return bitwise(aig, ql_aig_xor, left, right, width, output, error);
}

ql_status QL_CALL ql_aig_bv_add(ql_aig *aig, const ql_aig_lit *left,
                                const ql_aig_lit *right, ql_aig_lit carry_in,
                                uint32_t width, ql_aig_lit *output,
                                ql_aig_lit *carry_out, ql_error *error) {
    ql_aig_lit sum[AIG_TEMP_WIDTH];
    ql_aig_lit carry = carry_in;
    uint32_t index;
    ql_status status;

    if (left == NULL || right == NULL || output == NULL) {
        ql_error_set(error, QL_STATUS_INVALID_ARGUMENT,
                     "both operands and an output are required");
        return QL_STATUS_INVALID_ARGUMENT;
    }
    if (width == 0u || width > AIG_TEMP_WIDTH) {
        return check_width(width, error);
    }
    for (index = 0u; index < width; ++index) {
        ql_aig_lit half;
        ql_aig_lit and_ab;
        ql_aig_lit and_carry;

        status = ql_aig_xor(aig, left[index], right[index], &half, error);
        if (status != QL_STATUS_OK) {
            return status;
        }
        status = ql_aig_xor(aig, half, carry, &sum[index], error);
        if (status != QL_STATUS_OK) {
            return status;
        }
        status = ql_aig_and(aig, left[index], right[index], &and_ab, error);
        if (status != QL_STATUS_OK) {
            return status;
        }
        status = ql_aig_and(aig, half, carry, &and_carry, error);
        if (status != QL_STATUS_OK) {
            return status;
        }
        status = ql_aig_or(aig, and_ab, and_carry, &carry, error);
        if (status != QL_STATUS_OK) {
            return status;
        }
    }
    memcpy(output, sum, (size_t)width * sizeof(*output));
    if (carry_out != NULL) {
        *carry_out = carry;
    }
    return QL_STATUS_OK;
}

ql_status QL_CALL ql_aig_bv_sub(ql_aig *aig, const ql_aig_lit *left,
                                const ql_aig_lit *right, uint32_t width,
                                ql_aig_lit *output, ql_error *error) {
    ql_aig_lit complement[AIG_TEMP_WIDTH];
    uint32_t index;

    if (left == NULL || right == NULL || output == NULL) {
        ql_error_set(error, QL_STATUS_INVALID_ARGUMENT,
                     "both operands and an output are required");
        return QL_STATUS_INVALID_ARGUMENT;
    }
    if (width == 0u || width > AIG_TEMP_WIDTH) {
        return check_width(width, error);
    }
    for (index = 0u; index < width; ++index) {
        complement[index] = ql_aig_not(right[index]);
    }
    return ql_aig_bv_add(aig, left, complement, QL_AIG_LIT_TRUE, width, output,
                         NULL, error);
}

ql_status QL_CALL ql_aig_bv_neg(ql_aig *aig, const ql_aig_lit *operand,
                                uint32_t width, ql_aig_lit *output,
                                ql_error *error) {
    ql_aig_lit zero[AIG_TEMP_WIDTH];

    if (operand == NULL || output == NULL) {
        ql_error_set(error, QL_STATUS_INVALID_ARGUMENT,
                     "operand and output are required");
        return QL_STATUS_INVALID_ARGUMENT;
    }
    if (width == 0u || width > AIG_TEMP_WIDTH) {
        return check_width(width, error);
    }
    (void)ql_aig_bv_fill(QL_AIG_LIT_FALSE, width, zero);
    return ql_aig_bv_sub(aig, zero, operand, width, output, error);
}

ql_status QL_CALL ql_aig_bv_mul(ql_aig *aig, const ql_aig_lit *left,
                                const ql_aig_lit *right, uint32_t width,
                                ql_aig_lit *output, ql_error *error) {
    ql_aig_lit accumulator[QL_AIG_MAX_BIT_WIDTH];
    ql_aig_lit partial[QL_AIG_MAX_BIT_WIDTH];
    uint32_t step;
    ql_status status = check_width(width, error);

    if (left == NULL || right == NULL || output == NULL) {
        ql_error_set(error, QL_STATUS_INVALID_ARGUMENT,
                     "both operands and an output are required");
        return QL_STATUS_INVALID_ARGUMENT;
    }
    if (status != QL_STATUS_OK) {
        return status;
    }
    (void)ql_aig_bv_fill(QL_AIG_LIT_FALSE, width, accumulator);
    /* Shift and add, truncated to `width`. C multiplication is
       width-preserving for both signednesses, so one circuit serves both and
       the discarded high half is never built. */
    for (step = 0u; step < width; ++step) {
        uint32_t index;
        for (index = 0u; index < width; ++index) {
            if (index < step) {
                partial[index] = QL_AIG_LIT_FALSE;
                continue;
            }
            status = ql_aig_and(aig, left[index - step], right[step],
                                &partial[index], error);
            if (status != QL_STATUS_OK) {
                return status;
            }
        }
        status = ql_aig_bv_add(aig, accumulator, partial, QL_AIG_LIT_FALSE,
                               width, accumulator, NULL, error);
        if (status != QL_STATUS_OK) {
            return status;
        }
    }
    memcpy(output, accumulator, (size_t)width * sizeof(*output));
    return QL_STATUS_OK;
}

/* --- Comparisons ---------------------------------------------------------- */

ql_status QL_CALL ql_aig_bv_eq(ql_aig *aig, const ql_aig_lit *left,
                               const ql_aig_lit *right, uint32_t width,
                               ql_aig_lit *output, ql_error *error) {
    ql_aig_lit equal = QL_AIG_LIT_TRUE;
    uint32_t index;
    ql_status status = check_width(width, error);

    if (left == NULL || right == NULL || output == NULL) {
        ql_error_set(error, QL_STATUS_INVALID_ARGUMENT,
                     "both operands and an output are required");
        return QL_STATUS_INVALID_ARGUMENT;
    }
    if (status != QL_STATUS_OK) {
        return status;
    }
    for (index = 0u; index < width; ++index) {
        ql_aig_lit same;
        status = ql_aig_xnor(aig, left[index], right[index], &same, error);
        if (status != QL_STATUS_OK) {
            return status;
        }
        status = ql_aig_and(aig, equal, same, &equal, error);
        if (status != QL_STATUS_OK) {
            return status;
        }
    }
    *output = equal;
    return QL_STATUS_OK;
}

/* The restoring divider compares one bit wider than the public bound allows,
   so the unchecked form exists for it and the public entry point is a checked
   wrapper. */
static ql_status bv_ult_unchecked(ql_aig *aig, const ql_aig_lit *left,
                                  const ql_aig_lit *right, uint32_t width,
                                  ql_aig_lit *output, ql_error *error) {
    ql_aig_lit less = QL_AIG_LIT_FALSE;
    uint32_t index;
    ql_status status;

    /* Least significant bit first: at each position the more significant
       decision wins, and equal bits carry the decision made below. */
    for (index = 0u; index < width; ++index) {
        ql_aig_lit strictly_less;
        ql_aig_lit same;
        ql_aig_lit carried;

        status = ql_aig_and(aig, ql_aig_not(left[index]), right[index],
                            &strictly_less, error);
        if (status != QL_STATUS_OK) {
            return status;
        }
        status = ql_aig_xnor(aig, left[index], right[index], &same, error);
        if (status != QL_STATUS_OK) {
            return status;
        }
        status = ql_aig_and(aig, same, less, &carried, error);
        if (status != QL_STATUS_OK) {
            return status;
        }
        status = ql_aig_or(aig, strictly_less, carried, &less, error);
        if (status != QL_STATUS_OK) {
            return status;
        }
    }
    *output = less;
    return QL_STATUS_OK;
}

ql_status QL_CALL ql_aig_bv_ult(ql_aig *aig, const ql_aig_lit *left,
                                const ql_aig_lit *right, uint32_t width,
                                ql_aig_lit *output, ql_error *error) {
    ql_status status = check_width(width, error);

    if (left == NULL || right == NULL || output == NULL) {
        ql_error_set(error, QL_STATUS_INVALID_ARGUMENT,
                     "both operands and an output are required");
        return QL_STATUS_INVALID_ARGUMENT;
    }
    if (status != QL_STATUS_OK) {
        return status;
    }
    return bv_ult_unchecked(aig, left, right, width, output, error);
}

ql_status QL_CALL ql_aig_bv_ule(ql_aig *aig, const ql_aig_lit *left,
                                const ql_aig_lit *right, uint32_t width,
                                ql_aig_lit *output, ql_error *error) {
    ql_status status = ql_aig_bv_ult(aig, right, left, width, output, error);
    if (status == QL_STATUS_OK) {
        *output = ql_aig_not(*output);
    }
    return status;
}

/* Signed comparison is the unsigned one on the operands with their sign bits
   flipped, which maps the two's-complement order onto the unsigned order. */
static ql_status flip_sign_bit(const ql_aig_lit *operand, uint32_t width,
                               ql_aig_lit *output) {
    memcpy(output, operand, (size_t)width * sizeof(*output));
    output[width - 1u] = ql_aig_not(output[width - 1u]);
    return QL_STATUS_OK;
}

ql_status QL_CALL ql_aig_bv_slt(ql_aig *aig, const ql_aig_lit *left,
                                const ql_aig_lit *right, uint32_t width,
                                ql_aig_lit *output, ql_error *error) {
    ql_aig_lit biased_left[QL_AIG_MAX_BIT_WIDTH];
    ql_aig_lit biased_right[QL_AIG_MAX_BIT_WIDTH];
    ql_status status = check_width(width, error);

    if (left == NULL || right == NULL || output == NULL) {
        ql_error_set(error, QL_STATUS_INVALID_ARGUMENT,
                     "both operands and an output are required");
        return QL_STATUS_INVALID_ARGUMENT;
    }
    if (status != QL_STATUS_OK) {
        return status;
    }
    (void)flip_sign_bit(left, width, biased_left);
    (void)flip_sign_bit(right, width, biased_right);
    return ql_aig_bv_ult(aig, biased_left, biased_right, width, output, error);
}

ql_status QL_CALL ql_aig_bv_sle(ql_aig *aig, const ql_aig_lit *left,
                                const ql_aig_lit *right, uint32_t width,
                                ql_aig_lit *output, ql_error *error) {
    ql_status status = ql_aig_bv_slt(aig, right, left, width, output, error);
    if (status == QL_STATUS_OK) {
        *output = ql_aig_not(*output);
    }
    return status;
}

ql_status QL_CALL ql_aig_bv_reduce_or(ql_aig *aig, const ql_aig_lit *operand,
                                      uint32_t width, ql_aig_lit *output,
                                      ql_error *error) {
    ql_aig_lit any = QL_AIG_LIT_FALSE;
    uint32_t index;
    ql_status status = check_width(width, error);

    if (operand == NULL || output == NULL) {
        ql_error_set(error, QL_STATUS_INVALID_ARGUMENT,
                     "operand and output are required");
        return QL_STATUS_INVALID_ARGUMENT;
    }
    if (status != QL_STATUS_OK) {
        return status;
    }
    for (index = 0u; index < width; ++index) {
        status = ql_aig_or(aig, any, operand[index], &any, error);
        if (status != QL_STATUS_OK) {
            return status;
        }
    }
    *output = any;
    return QL_STATUS_OK;
}

ql_status QL_CALL ql_aig_bv_reduce_and(ql_aig *aig, const ql_aig_lit *operand,
                                       uint32_t width, ql_aig_lit *output,
                                       ql_error *error) {
    ql_aig_lit all = QL_AIG_LIT_TRUE;
    uint32_t index;
    ql_status status = check_width(width, error);

    if (operand == NULL || output == NULL) {
        ql_error_set(error, QL_STATUS_INVALID_ARGUMENT,
                     "operand and output are required");
        return QL_STATUS_INVALID_ARGUMENT;
    }
    if (status != QL_STATUS_OK) {
        return status;
    }
    for (index = 0u; index < width; ++index) {
        status = ql_aig_and(aig, all, operand[index], &all, error);
        if (status != QL_STATUS_OK) {
            return status;
        }
    }
    *output = all;
    return QL_STATUS_OK;
}

/* --- Width changes -------------------------------------------------------- */

ql_status QL_CALL ql_aig_bv_zext(ql_aig *aig, const ql_aig_lit *operand,
                                 uint32_t from_width, uint32_t to_width,
                                 ql_aig_lit *output, ql_error *error) {
    ql_aig_lit widened[QL_AIG_MAX_BIT_WIDTH];
    uint32_t index;
    ql_status status;

    (void)aig;
    if (operand == NULL || output == NULL) {
        ql_error_set(error, QL_STATUS_INVALID_ARGUMENT,
                     "operand and output are required");
        return QL_STATUS_INVALID_ARGUMENT;
    }
    status = check_width(from_width, error);
    if (status == QL_STATUS_OK) {
        status = check_width(to_width, error);
    }
    if (status != QL_STATUS_OK) {
        return status;
    }
    if (to_width < from_width) {
        ql_error_set(error, QL_STATUS_INVALID_ARGUMENT,
                     "an extension cannot narrow %u bits to %u", from_width,
                     to_width);
        return QL_STATUS_INVALID_ARGUMENT;
    }
    for (index = 0u; index < to_width; ++index) {
        widened[index] =
            index < from_width ? operand[index] : QL_AIG_LIT_FALSE;
    }
    memcpy(output, widened, (size_t)to_width * sizeof(*output));
    return QL_STATUS_OK;
}

ql_status QL_CALL ql_aig_bv_sext(ql_aig *aig, const ql_aig_lit *operand,
                                 uint32_t from_width, uint32_t to_width,
                                 ql_aig_lit *output, ql_error *error) {
    ql_aig_lit widened[QL_AIG_MAX_BIT_WIDTH];
    uint32_t index;
    ql_status status;

    (void)aig;
    if (operand == NULL || output == NULL) {
        ql_error_set(error, QL_STATUS_INVALID_ARGUMENT,
                     "operand and output are required");
        return QL_STATUS_INVALID_ARGUMENT;
    }
    status = check_width(from_width, error);
    if (status == QL_STATUS_OK) {
        status = check_width(to_width, error);
    }
    if (status != QL_STATUS_OK) {
        return status;
    }
    if (to_width < from_width) {
        ql_error_set(error, QL_STATUS_INVALID_ARGUMENT,
                     "an extension cannot narrow %u bits to %u", from_width,
                     to_width);
        return QL_STATUS_INVALID_ARGUMENT;
    }
    for (index = 0u; index < to_width; ++index) {
        widened[index] = index < from_width ? operand[index]
                                            : operand[from_width - 1u];
    }
    memcpy(output, widened, (size_t)to_width * sizeof(*output));
    return QL_STATUS_OK;
}

ql_status QL_CALL ql_aig_bv_trunc(ql_aig *aig, const ql_aig_lit *operand,
                                  uint32_t from_width, uint32_t to_width,
                                  ql_aig_lit *output, ql_error *error) {
    ql_aig_lit narrowed[QL_AIG_MAX_BIT_WIDTH];
    ql_status status;

    (void)aig;
    if (operand == NULL || output == NULL) {
        ql_error_set(error, QL_STATUS_INVALID_ARGUMENT,
                     "operand and output are required");
        return QL_STATUS_INVALID_ARGUMENT;
    }
    status = check_width(from_width, error);
    if (status == QL_STATUS_OK) {
        status = check_width(to_width, error);
    }
    if (status != QL_STATUS_OK) {
        return status;
    }
    if (to_width > from_width) {
        ql_error_set(error, QL_STATUS_INVALID_ARGUMENT,
                     "a truncation cannot widen %u bits to %u", from_width,
                     to_width);
        return QL_STATUS_INVALID_ARGUMENT;
    }
    memcpy(narrowed, operand, (size_t)to_width * sizeof(*narrowed));
    memcpy(output, narrowed, (size_t)to_width * sizeof(*output));
    return QL_STATUS_OK;
}

static ql_status bv_mux_unchecked(ql_aig *aig, ql_aig_lit condition,
                                  const ql_aig_lit *when_true,
                                  const ql_aig_lit *when_false,
                                  uint32_t width, ql_aig_lit *output,
                                  ql_error *error) {
    ql_aig_lit chosen[AIG_TEMP_WIDTH];
    uint32_t index;
    ql_status status;

    for (index = 0u; index < width; ++index) {
        status = ql_aig_mux(aig, condition, when_true[index],
                            when_false[index], &chosen[index], error);
        if (status != QL_STATUS_OK) {
            return status;
        }
    }
    memcpy(output, chosen, (size_t)width * sizeof(*output));
    return QL_STATUS_OK;
}

ql_status QL_CALL ql_aig_bv_mux(ql_aig *aig, ql_aig_lit condition,
                                const ql_aig_lit *when_true,
                                const ql_aig_lit *when_false, uint32_t width,
                                ql_aig_lit *output, ql_error *error) {
    ql_status status = check_width(width, error);

    if (when_true == NULL || when_false == NULL || output == NULL) {
        ql_error_set(error, QL_STATUS_INVALID_ARGUMENT,
                     "both arms and an output are required");
        return QL_STATUS_INVALID_ARGUMENT;
    }
    if (status != QL_STATUS_OK) {
        return status;
    }
    return bv_mux_unchecked(aig, condition, when_true, when_false, width,
                            output, error);
}

/* --- Shifts --------------------------------------------------------------- */

/* The number of shift-amount bits a barrel shifter over `width` needs. Higher
   amount bits can only mean an out-of-range shift, which the range guard
   handles wholesale. */
static uint32_t barrel_stages(uint32_t width) {
    uint32_t stages = 0u;
    while ((UINT32_C(1) << stages) < width) {
        ++stages;
    }
    return stages;
}

/* The value `width` as a width-bit constant. It always fits: width is at most
   2^width - 1 for every width of at least one. */
static void width_constant(uint32_t width, ql_aig_lit *output) {
    uint32_t index;

    for (index = 0u; index < width; ++index) {
        output[index] = ((width >> index) & 1u) != 0u ? QL_AIG_LIT_TRUE
                                                      : QL_AIG_LIT_FALSE;
    }
}

typedef enum shift_kind { SHIFT_LEFT, SHIFT_LOGICAL_RIGHT, SHIFT_ARITHMETIC_RIGHT } shift_kind;

static ql_status barrel_shift(ql_aig *aig, shift_kind kind,
                              const ql_aig_lit *operand,
                              const ql_aig_lit *amount, uint32_t width,
                              ql_aig_lit *output, ql_error *error) {
    ql_aig_lit current[QL_AIG_MAX_BIT_WIDTH];
    ql_aig_lit shifted[QL_AIG_MAX_BIT_WIDTH];
    ql_aig_lit saturated[QL_AIG_MAX_BIT_WIDTH];
    ql_aig_lit limit[QL_AIG_MAX_BIT_WIDTH];
    const ql_aig_lit fill = kind == SHIFT_ARITHMETIC_RIGHT
                                ? operand[width - 1u]
                                : QL_AIG_LIT_FALSE;
    const uint32_t stages = barrel_stages(width);
    ql_aig_lit in_range;
    uint32_t stage;
    ql_status status;

    memcpy(current, operand, (size_t)width * sizeof(*current));
    for (stage = 0u; stage < stages; ++stage) {
        const uint32_t distance = UINT32_C(1) << stage;
        uint32_t index;

        for (index = 0u; index < width; ++index) {
            if (kind == SHIFT_LEFT) {
                shifted[index] =
                    index >= distance ? current[index - distance] : fill;
            } else {
                shifted[index] = index + distance < width
                                     ? current[index + distance]
                                     : fill;
            }
        }
        status = ql_aig_bv_mux(aig, amount[stage], shifted, current, width,
                               current, error);
        if (status != QL_STATUS_OK) {
            return status;
        }
    }
    /* An amount at or above the width is not undefined here. SMT-LIB
       totalizes it to zero, zero, or the sign bit, and this reproduces that.
       C's undefined over-shift is the UB guard's business, not this
       circuit's. */
    width_constant(width, limit);
    status = ql_aig_bv_ult(aig, amount, limit, width, &in_range, error);
    if (status != QL_STATUS_OK) {
        return status;
    }
    (void)ql_aig_bv_fill(fill, width, saturated);
    return ql_aig_bv_mux(aig, in_range, current, saturated, width, output,
                         error);
}

ql_status QL_CALL ql_aig_bv_shl(ql_aig *aig, const ql_aig_lit *operand,
                                const ql_aig_lit *amount, uint32_t width,
                                ql_aig_lit *output, ql_error *error) {
    ql_status status = check_width(width, error);

    if (operand == NULL || amount == NULL || output == NULL) {
        ql_error_set(error, QL_STATUS_INVALID_ARGUMENT,
                     "operand, amount, and output are required");
        return QL_STATUS_INVALID_ARGUMENT;
    }
    if (status != QL_STATUS_OK) {
        return status;
    }
    return barrel_shift(aig, SHIFT_LEFT, operand, amount, width, output,
                        error);
}

ql_status QL_CALL ql_aig_bv_lshr(ql_aig *aig, const ql_aig_lit *operand,
                                 const ql_aig_lit *amount, uint32_t width,
                                 ql_aig_lit *output, ql_error *error) {
    ql_status status = check_width(width, error);

    if (operand == NULL || amount == NULL || output == NULL) {
        ql_error_set(error, QL_STATUS_INVALID_ARGUMENT,
                     "operand, amount, and output are required");
        return QL_STATUS_INVALID_ARGUMENT;
    }
    if (status != QL_STATUS_OK) {
        return status;
    }
    return barrel_shift(aig, SHIFT_LOGICAL_RIGHT, operand, amount, width,
                        output, error);
}

ql_status QL_CALL ql_aig_bv_ashr(ql_aig *aig, const ql_aig_lit *operand,
                                 const ql_aig_lit *amount, uint32_t width,
                                 ql_aig_lit *output, ql_error *error) {
    ql_status status = check_width(width, error);

    if (operand == NULL || amount == NULL || output == NULL) {
        ql_error_set(error, QL_STATUS_INVALID_ARGUMENT,
                     "operand, amount, and output are required");
        return QL_STATUS_INVALID_ARGUMENT;
    }
    if (status != QL_STATUS_OK) {
        return status;
    }
    return barrel_shift(aig, SHIFT_ARITHMETIC_RIGHT, operand, amount, width,
                        output, error);
}

/* --- Division ------------------------------------------------------------- */

ql_status QL_CALL ql_aig_bv_udivrem(ql_aig *aig, const ql_aig_lit *dividend,
                                    const ql_aig_lit *divisor, uint32_t width,
                                    ql_aig_lit *quotient,
                                    ql_aig_lit *remainder, ql_error *error) {
    /* Restoring division, most significant bit first. The running remainder
       carries one extra bit so that the shift-in cannot overflow it. */
    ql_aig_lit rest[AIG_TEMP_WIDTH];
    ql_aig_lit wide_divisor[AIG_TEMP_WIDTH];
    ql_aig_lit difference[AIG_TEMP_WIDTH];
    ql_aig_lit bits[QL_AIG_MAX_BIT_WIDTH];
    const uint32_t wide = width + 1u;
    uint32_t step;
    ql_status status = check_width(width, error);

    if (dividend == NULL || divisor == NULL) {
        ql_error_set(error, QL_STATUS_INVALID_ARGUMENT,
                     "both operands are required");
        return QL_STATUS_INVALID_ARGUMENT;
    }
    if (status != QL_STATUS_OK) {
        return status;
    }
    (void)ql_aig_bv_fill(QL_AIG_LIT_FALSE, wide, rest);
    memcpy(wide_divisor, divisor, (size_t)width * sizeof(*wide_divisor));
    wide_divisor[width] = QL_AIG_LIT_FALSE;

    for (step = 0u; step < width; ++step) {
        const uint32_t bit = width - 1u - step;
        ql_aig_lit too_small;
        uint32_t index;

        for (index = wide - 1u; index > 0u; --index) {
            rest[index] = rest[index - 1u];
        }
        rest[0] = dividend[bit];

        status = bv_ult_unchecked(aig, rest, wide_divisor, wide, &too_small,
                                  error);
        if (status != QL_STATUS_OK) {
            return status;
        }
        /* A zero divisor makes every step subtract nothing and set every
           quotient bit, which is exactly the SMT-LIB totalization: an all-ones
           quotient and the dividend as the remainder. Rejecting the division
           is the UB guard's job, and this circuit must not be read as
           deciding it. */
        status = ql_aig_bv_sub(aig, rest, wide_divisor, wide, difference,
                               error);
        if (status != QL_STATUS_OK) {
            return status;
        }
        status = bv_mux_unchecked(aig, too_small, rest, difference, wide, rest,
                                  error);
        if (status != QL_STATUS_OK) {
            return status;
        }
        bits[bit] = ql_aig_not(too_small);
    }
    if (quotient != NULL) {
        memcpy(quotient, bits, (size_t)width * sizeof(*quotient));
    }
    if (remainder != NULL) {
        memcpy(remainder, rest, (size_t)width * sizeof(*remainder));
    }
    return QL_STATUS_OK;
}

ql_status QL_CALL ql_aig_bv_sdivrem(ql_aig *aig, const ql_aig_lit *dividend,
                                    const ql_aig_lit *divisor, uint32_t width,
                                    ql_aig_lit *quotient,
                                    ql_aig_lit *remainder, ql_error *error) {
    ql_aig_lit magnitude_dividend[QL_AIG_MAX_BIT_WIDTH];
    ql_aig_lit magnitude_divisor[QL_AIG_MAX_BIT_WIDTH];
    ql_aig_lit negated[QL_AIG_MAX_BIT_WIDTH];
    ql_aig_lit unsigned_quotient[QL_AIG_MAX_BIT_WIDTH];
    ql_aig_lit unsigned_remainder[QL_AIG_MAX_BIT_WIDTH];
    ql_aig_lit dividend_negative;
    ql_aig_lit divisor_negative;
    ql_aig_lit signs_differ;
    ql_status status = check_width(width, error);

    if (dividend == NULL || divisor == NULL) {
        ql_error_set(error, QL_STATUS_INVALID_ARGUMENT,
                     "both operands are required");
        return QL_STATUS_INVALID_ARGUMENT;
    }
    if (status != QL_STATUS_OK) {
        return status;
    }
    dividend_negative = dividend[width - 1u];
    divisor_negative = divisor[width - 1u];

    /* Divide magnitudes, then reapply signs. This is truncation toward zero,
       which is what both C and SMT-LIB bvsdiv/bvsrem specify. */
    status = ql_aig_bv_neg(aig, dividend, width, negated, error);
    if (status != QL_STATUS_OK) {
        return status;
    }
    status = ql_aig_bv_mux(aig, dividend_negative, negated, dividend, width,
                           magnitude_dividend, error);
    if (status != QL_STATUS_OK) {
        return status;
    }
    status = ql_aig_bv_neg(aig, divisor, width, negated, error);
    if (status != QL_STATUS_OK) {
        return status;
    }
    status = ql_aig_bv_mux(aig, divisor_negative, negated, divisor, width,
                           magnitude_divisor, error);
    if (status != QL_STATUS_OK) {
        return status;
    }
    status = ql_aig_bv_udivrem(aig, magnitude_dividend, magnitude_divisor,
                               width, unsigned_quotient, unsigned_remainder,
                               error);
    if (status != QL_STATUS_OK) {
        return status;
    }
    if (quotient != NULL) {
        status = ql_aig_xor(aig, dividend_negative, divisor_negative,
                            &signs_differ, error);
        if (status != QL_STATUS_OK) {
            return status;
        }
        status = ql_aig_bv_neg(aig, unsigned_quotient, width, negated, error);
        if (status != QL_STATUS_OK) {
            return status;
        }
        status = ql_aig_bv_mux(aig, signs_differ, negated, unsigned_quotient,
                               width, quotient, error);
        if (status != QL_STATUS_OK) {
            return status;
        }
    }
    if (remainder != NULL) {
        /* The remainder takes the dividend's sign, so that quotient * divisor
           plus remainder is the dividend. */
        status = ql_aig_bv_neg(aig, unsigned_remainder, width, negated, error);
        if (status != QL_STATUS_OK) {
            return status;
        }
        status = ql_aig_bv_mux(aig, dividend_negative, negated,
                               unsigned_remainder, width, remainder, error);
        if (status != QL_STATUS_OK) {
            return status;
        }
    }
    return QL_STATUS_OK;
}

/* --- Evaluation ----------------------------------------------------------- */

ql_status QL_CALL ql_aig_evaluate(const ql_aig *aig,
                                  const uint8_t *input_values,
                                  size_t input_count, ql_aig_lit literal,
                                  uint32_t *output, ql_error *error) {
    uint8_t *values;
    size_t index;

    if (aig == NULL || output == NULL) {
        ql_error_set(error, QL_STATUS_INVALID_ARGUMENT,
                     "graph and output are required");
        return QL_STATUS_INVALID_ARGUMENT;
    }
    *output = 0u;
    if (!ql_aig_literal_is_valid(aig, literal)) {
        ql_error_set(error, QL_STATUS_INVALID_ARGUMENT,
                     "the literal does not belong to this graph");
        return QL_STATUS_INVALID_ARGUMENT;
    }
    if (input_count != aig->input_count) {
        ql_error_set(error, QL_STATUS_INVALID_ARGUMENT,
                     "the graph has %zu inputs but %zu values were supplied",
                     aig->input_count, input_count);
        return QL_STATUS_INVALID_ARGUMENT;
    }
    if (input_count != 0u && input_values == NULL) {
        ql_error_set(error, QL_STATUS_INVALID_ARGUMENT,
                     "input values are required");
        return QL_STATUS_INVALID_ARGUMENT;
    }
    values = aig->allocator.allocate(aig->allocator.user_data,
                                     aig->node_count);
    if (values == NULL) {
        ql_error_set(error, QL_STATUS_OUT_OF_MEMORY, NULL);
        return QL_STATUS_OUT_OF_MEMORY;
    }
    /* Operands always have a lower index than the node that uses them, so a
       single forward sweep is a topological evaluation and no recursion can
       overflow a stack on a deep adder chain. */
    values[0] = 0u;
    for (index = 1u; index < aig->node_count; ++index) {
        const aig_node *node = &aig->nodes[index];
        uint8_t left;
        uint8_t right;

        if (node->is_input != 0u) {
            values[index] =
                input_values[node->input_index] != 0u ? 1u : 0u;
            continue;
        }
        left = (uint8_t)(values[literal_to_node(node->left)] ^
                         (uint8_t)(node->left & 1u));
        right = (uint8_t)(values[literal_to_node(node->right)] ^
                          (uint8_t)(node->right & 1u));
        values[index] = (uint8_t)(left & right);
    }
    *output = (uint32_t)(values[literal_to_node(literal)] ^
                         (uint32_t)(literal & 1u));
    aig->allocator.deallocate(aig->allocator.user_data, values);
    ql_error_clear(error);
    return QL_STATUS_OK;
}

/* --- CNF ------------------------------------------------------------------ */

struct ql_aig_cnf {
    ql_allocator allocator;
    /* Node index to DIMACS variable, zero when the root's cone misses it. */
    uint32_t *variables;
    size_t node_count;
    uint32_t *input_variables;
    size_t input_count;
    int32_t *clauses;
    size_t clause_literal_count;
    size_t clause_literal_capacity;
    uint64_t variable_count;
    uint64_t clause_count;
    uint32_t trivially_true;
    uint32_t trivially_false;
};

static ql_status clause_reserve(ql_aig_cnf *cnf, size_t extra,
                                ql_error *error) {
    size_t capacity;
    int32_t *grown;

    if (cnf->clause_literal_count + extra <= cnf->clause_literal_capacity) {
        return QL_STATUS_OK;
    }
    capacity = cnf->clause_literal_capacity == 0u
                   ? 1024u
                   : cnf->clause_literal_capacity;
    while (capacity < cnf->clause_literal_count + extra) {
        capacity *= 2u;
    }
    grown = cnf->allocator.reallocate(cnf->allocator.user_data, cnf->clauses,
                                      capacity * sizeof(*grown));
    if (grown == NULL) {
        ql_error_set(error, QL_STATUS_OUT_OF_MEMORY, NULL);
        return QL_STATUS_OUT_OF_MEMORY;
    }
    cnf->clauses = grown;
    cnf->clause_literal_capacity = capacity;
    return QL_STATUS_OK;
}

/* Clauses are stored back to back, each terminated by a zero, exactly as
   DIMACS writes them. */
static ql_status clause_add(ql_aig_cnf *cnf, const int32_t *literals,
                            size_t count, ql_error *error) {
    ql_status status = clause_reserve(cnf, count + 1u, error);

    if (status != QL_STATUS_OK) {
        return status;
    }
    if (count != 0u) {
        memcpy(&cnf->clauses[cnf->clause_literal_count], literals,
               count * sizeof(*literals));
    }
    cnf->clause_literal_count += count;
    cnf->clauses[cnf->clause_literal_count] = 0;
    ++cnf->clause_literal_count;
    ++cnf->clause_count;
    return QL_STATUS_OK;
}

static int32_t cnf_literal(const ql_aig_cnf *cnf, ql_aig_lit literal) {
    const uint32_t variable = cnf->variables[literal_to_node(literal)];
    return (literal & 1u) != 0u ? -(int32_t)variable : (int32_t)variable;
}

void QL_CALL ql_aig_cnf_destroy(ql_aig_cnf *cnf) {
    ql_allocator allocator;

    if (cnf == NULL) {
        return;
    }
    allocator = cnf->allocator;
    allocator.deallocate(allocator.user_data, cnf->variables);
    allocator.deallocate(allocator.user_data, cnf->input_variables);
    allocator.deallocate(allocator.user_data, cnf->clauses);
    allocator.deallocate(allocator.user_data, cnf);
}

ql_status QL_CALL ql_aig_cnf_create(const ql_allocator *allocator,
                                    const ql_aig *aig, ql_aig_lit root,
                                    ql_aig_cnf **output, ql_error *error) {
    const ql_allocator *selected = select_allocator(allocator);
    ql_aig_cnf *cnf;
    uint8_t *reached = NULL;
    size_t index;
    int32_t unit[1];
    ql_status status;

    if (aig == NULL || output == NULL) {
        ql_error_set(error, QL_STATUS_INVALID_ARGUMENT,
                     "graph and output are required");
        return QL_STATUS_INVALID_ARGUMENT;
    }
    *output = NULL;
    if (!ql_aig_literal_is_valid(aig, root)) {
        ql_error_set(error, QL_STATUS_INVALID_ARGUMENT,
                     "the root literal does not belong to this graph");
        return QL_STATUS_INVALID_ARGUMENT;
    }
    if (!ql_allocator_is_valid(selected)) {
        ql_error_set(error, QL_STATUS_INVALID_ARGUMENT, "invalid allocator");
        return QL_STATUS_INVALID_ARGUMENT;
    }
    cnf = selected->allocate(selected->user_data, sizeof(*cnf));
    if (cnf == NULL) {
        ql_error_set(error, QL_STATUS_OUT_OF_MEMORY, NULL);
        return QL_STATUS_OUT_OF_MEMORY;
    }
    memset(cnf, 0, sizeof(*cnf));
    cnf->allocator = *selected;
    cnf->node_count = aig->node_count;
    cnf->input_count = aig->input_count;
    cnf->variables = selected->allocate(selected->user_data,
                                        aig->node_count *
                                            sizeof(*cnf->variables));
    reached = selected->allocate(selected->user_data, aig->node_count);
    if (cnf->variables == NULL || reached == NULL) {
        ql_error_set(error, QL_STATUS_OUT_OF_MEMORY, NULL);
        status = QL_STATUS_OUT_OF_MEMORY;
        goto cleanup;
    }
    memset(cnf->variables, 0, aig->node_count * sizeof(*cnf->variables));
    memset(reached, 0, aig->node_count);
    if (aig->input_count != 0u) {
        cnf->input_variables =
            selected->allocate(selected->user_data,
                               aig->input_count *
                                   sizeof(*cnf->input_variables));
        if (cnf->input_variables == NULL) {
            ql_error_set(error, QL_STATUS_OUT_OF_MEMORY, NULL);
            status = QL_STATUS_OUT_OF_MEMORY;
            goto cleanup;
        }
        memset(cnf->input_variables, 0,
               aig->input_count * sizeof(*cnf->input_variables));
    }

    if (ql_aig_is_constant(root) != 0u) {
        /* A folded root needs no solver at all. It is still an answer this
           encoding produced and never a checked proof. */
        cnf->trivially_true = root == QL_AIG_LIT_TRUE ? 1u : 0u;
        cnf->trivially_false = root == QL_AIG_LIT_FALSE ? 1u : 0u;
        if (cnf->trivially_false != 0u) {
            status = clause_add(cnf, NULL, 0u, error);
            if (status != QL_STATUS_OK) {
                goto cleanup;
            }
        }
        selected->deallocate(selected->user_data, reached);
        *output = cnf;
        ql_error_clear(error);
        return QL_STATUS_OK;
    }

    /* Operands have lower indices than their user, so one descending sweep
       computes the cone of influence exactly. Nodes outside it never get a
       variable, and a half of a wide circuit the root does not use costs
       nothing. */
    reached[literal_to_node(root)] = 1u;
    for (index = aig->node_count; index-- > 1u;) {
        const aig_node *node = &aig->nodes[index];
        if (reached[index] == 0u || node->is_input != 0u) {
            continue;
        }
        reached[literal_to_node(node->left)] = 1u;
        reached[literal_to_node(node->right)] = 1u;
    }
    for (index = 1u; index < aig->node_count; ++index) {
        if (reached[index] == 0u) {
            continue;
        }
        ++cnf->variable_count;
        cnf->variables[index] = (uint32_t)cnf->variable_count;
        if (aig->nodes[index].is_input != 0u) {
            cnf->input_variables[aig->nodes[index].input_index] =
                cnf->variables[index];
        }
    }
    for (index = 1u; index < aig->node_count; ++index) {
        const aig_node *node = &aig->nodes[index];
        int32_t clause[3];
        int32_t self;
        int32_t left;
        int32_t right;

        if (reached[index] == 0u || node->is_input != 0u) {
            continue;
        }
        self = (int32_t)cnf->variables[index];
        left = cnf_literal(cnf, node->left);
        right = cnf_literal(cnf, node->right);
        clause[0] = -self;
        clause[1] = left;
        status = clause_add(cnf, clause, 2u, error);
        if (status != QL_STATUS_OK) {
            goto cleanup;
        }
        clause[1] = right;
        status = clause_add(cnf, clause, 2u, error);
        if (status != QL_STATUS_OK) {
            goto cleanup;
        }
        clause[0] = self;
        clause[1] = -left;
        clause[2] = -right;
        status = clause_add(cnf, clause, 3u, error);
        if (status != QL_STATUS_OK) {
            goto cleanup;
        }
    }
    unit[0] = cnf_literal(cnf, root);
    status = clause_add(cnf, unit, 1u, error);
    if (status != QL_STATUS_OK) {
        goto cleanup;
    }
    selected->deallocate(selected->user_data, reached);
    *output = cnf;
    ql_error_clear(error);
    return QL_STATUS_OK;

cleanup:
    selected->deallocate(selected->user_data, reached);
    ql_aig_cnf_destroy(cnf);
    return status;
}

ql_status QL_CALL ql_aig_cnf_get_view(const ql_aig_cnf *cnf,
                                      ql_aig_cnf_view_v1 *view,
                                      ql_error *error) {
    if (cnf == NULL || view == NULL) {
        ql_error_set(error, QL_STATUS_INVALID_ARGUMENT,
                     "formula and view are required");
        return QL_STATUS_INVALID_ARGUMENT;
    }
    if (view->struct_size != 0u && view->struct_size < sizeof(*view)) {
        ql_error_set(error, QL_STATUS_ABI_MISMATCH,
                     "CNF view structure is too small");
        return QL_STATUS_ABI_MISMATCH;
    }
    memset(view, 0, sizeof(*view));
    view->struct_size = sizeof(*view);
    view->schema_version = QL_AIG_SCHEMA_VERSION;
    view->variable_count = cnf->variable_count;
    view->clause_count = cnf->clause_count;
    view->trivially_true = cnf->trivially_true;
    view->trivially_false = cnf->trivially_false;
    ql_error_clear(error);
    return QL_STATUS_OK;
}

uint32_t QL_CALL ql_aig_cnf_input_variable(const ql_aig_cnf *cnf,
                                           uint32_t input_index) {
    if (cnf == NULL || (size_t)input_index >= cnf->input_count ||
        cnf->input_variables == NULL) {
        return 0u;
    }
    return cnf->input_variables[input_index];
}

ql_status QL_CALL ql_aig_cnf_artifact_create(const ql_allocator *allocator,
                                             const ql_aig_cnf *cnf,
                                             ql_artifact **output,
                                             ql_error *error) {
    const ql_allocator *selected = select_allocator(allocator);
    char header[64];
    char *text = NULL;
    size_t capacity;
    size_t size = 0u;
    size_t index;
    int written;
    ql_status status;

    if (cnf == NULL || output == NULL) {
        ql_error_set(error, QL_STATUS_INVALID_ARGUMENT,
                     "formula and output are required");
        return QL_STATUS_INVALID_ARGUMENT;
    }
    *output = NULL;
    written = snprintf(header, sizeof(header), "p cnf %llu %llu\n",
                       (unsigned long long)cnf->variable_count,
                       (unsigned long long)cnf->clause_count);
    if (written < 0 || (size_t)written >= sizeof(header)) {
        ql_error_set(error, QL_STATUS_INTERNAL_ERROR,
                     "could not format the DIMACS header");
        return QL_STATUS_INTERNAL_ERROR;
    }
    /* Twelve characters is the widest a signed 32-bit literal plus its
       separator can be, so this bound never has to grow mid-write. */
    capacity = (size_t)written + cnf->clause_literal_count * 12u + 1u;
    text = selected->allocate(selected->user_data, capacity);
    if (text == NULL) {
        ql_error_set(error, QL_STATUS_OUT_OF_MEMORY, NULL);
        return QL_STATUS_OUT_OF_MEMORY;
    }
    memcpy(text, header, (size_t)written);
    size = (size_t)written;
    for (index = 0u; index < cnf->clause_literal_count; ++index) {
        const int32_t literal = cnf->clauses[index];
        written = snprintf(text + size, capacity - size, "%ld%s",
                           (long)literal, literal == 0 ? "\n" : " ");
        if (written < 0 || (size_t)written >= capacity - size) {
            selected->deallocate(selected->user_data, text);
            ql_error_set(error, QL_STATUS_INTERNAL_ERROR,
                         "the DIMACS buffer was sized too small");
            return QL_STATUS_INTERNAL_ERROR;
        }
        size += (size_t)written;
    }
    status = ql_artifact_create(selected, QL_ARTIFACT_KIND_DIMACS_CNF,
                                QL_AIG_SCHEMA_VERSION, text, size, output,
                                error);
    selected->deallocate(selected->user_data, text);
    return status;
}
