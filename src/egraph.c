#include "quodlibet/egraph.h"

#include <limits.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "blake3.h"

#define QL_EGRAPH_DEFAULT_MAX_TERMS UINT64_C(1048576)
#define QL_EGRAPH_DEFAULT_MAX_MERGES UINT64_C(4194304)
#define QL_EGRAPH_DEFAULT_MAX_BIT_WIDTH UINT32_C(4096)
#define QL_EGRAPH_DEFAULT_MAX_SYMBOL_BYTES UINT32_C(1024)
#define QL_EGRAPH_DEFAULT_MAX_ITERATIONS UINT32_C(32)
#define QL_EGRAPH_DEFAULT_MAX_REWRITES UINT64_C(1048576)

typedef struct ql_egraph_node {
    ql_egraph_type type;
    ql_egraph_operator op;
    uint32_t operand_count;
    ql_egraph_term_id operands[QL_EGRAPH_MAX_ARITY];
    ql_egraph_class_id initial_class;
    char *symbol;
    uint8_t *constant_le;
    size_t constant_size;
} ql_egraph_node;

typedef struct ql_egraph_class {
    ql_egraph_class_id parent;
} ql_egraph_class;

struct ql_egraph {
    ql_allocator allocator;
    ql_egraph_config_v1 config;
    ql_egraph_node *nodes;
    size_t node_count;
    size_t node_capacity;
    ql_egraph_class *classes;
    size_t class_count;
    size_t class_capacity;
    ql_egraph_merge_record_v1 *merges;
    size_t merge_count;
    size_t merge_capacity;
    uint64_t axiom_merge_count;
    ql_egraph_term_id *exact_table;
    size_t exact_capacity;
    ql_egraph_stop_reason last_resource_stop;
};

static uint64_t hash_byte(uint64_t hash, uint8_t byte) {
    return (hash ^ (uint64_t)byte) * UINT64_C(1099511628211);
}

static uint64_t hash_u32(uint64_t hash, uint32_t value) {
    unsigned int index;
    for (index = 0u; index < 4u; ++index) {
        hash = hash_byte(hash, (uint8_t)(value & UINT32_C(255)));
        value >>= 8u;
    }
    return hash;
}

static uint64_t hash_bytes(uint64_t hash, const uint8_t *bytes,
                           size_t count) {
    size_t index;
    for (index = 0u; index < count; ++index) {
        hash = hash_byte(hash, bytes[index]);
    }
    return hash;
}

static int types_equal(ql_egraph_type lhs, ql_egraph_type rhs) {
    return lhs.kind == rhs.kind && lhs.bit_width == rhs.bit_width;
}

static ql_egraph_type bool_type(void) {
    ql_egraph_type type;
    type.kind = QL_EGRAPH_SORT_BOOL;
    type.bit_width = 1u;
    return type;
}

static ql_egraph_type bv_type(uint32_t width) {
    ql_egraph_type type;
    type.kind = QL_EGRAPH_SORT_BITVECTOR;
    type.bit_width = width;
    return type;
}

static int valid_type(const ql_egraph *graph, ql_egraph_type type) {
    if (type.kind == QL_EGRAPH_SORT_BOOL) {
        return type.bit_width == 1u;
    }
    return type.kind == QL_EGRAPH_SORT_BITVECTOR && type.bit_width != 0u &&
           type.bit_width <= graph->config.max_bit_width;
}

static ql_egraph_class_id find_root(ql_egraph *graph,
                                    ql_egraph_class_id class_id) {
    ql_egraph_class_id root = class_id;
    ql_egraph_class_id current;

    while (graph->classes[(size_t)root - 1u].parent != root) {
        root = graph->classes[(size_t)root - 1u].parent;
    }
    current = class_id;
    while (graph->classes[(size_t)current - 1u].parent != current) {
        const ql_egraph_class_id next =
            graph->classes[(size_t)current - 1u].parent;
        graph->classes[(size_t)current - 1u].parent = root;
        current = next;
    }
    return root;
}

static ql_egraph_class_id find_root_const(
    const ql_egraph *graph, ql_egraph_class_id class_id) {
    while (graph->classes[(size_t)class_id - 1u].parent != class_id) {
        class_id = graph->classes[(size_t)class_id - 1u].parent;
    }
    return class_id;
}

static ql_status require_graph(const ql_egraph *graph, ql_error *error) {
    if (graph == NULL) {
        ql_error_set(error, QL_STATUS_INVALID_ARGUMENT,
                     "e-graph is required");
        return QL_STATUS_INVALID_ARGUMENT;
    }
    return QL_STATUS_OK;
}

static ql_status require_term(const ql_egraph *graph,
                              ql_egraph_term_id term, ql_error *error) {
    if (term == QL_EGRAPH_INVALID_TERM ||
        (uint64_t)term > (uint64_t)graph->node_count) {
        ql_error_set(error, QL_STATUS_INVALID_ARGUMENT,
                     "e-graph term %u does not exist", term);
        return QL_STATUS_INVALID_ARGUMENT;
    }
    return QL_STATUS_OK;
}

static ql_status grow_vector(ql_egraph *graph, void **storage,
                             size_t *capacity, size_t required,
                             size_t element_size, size_t hard_limit,
                             ql_egraph_stop_reason limit_reason,
                             const char *description, ql_error *error) {
    size_t next;
    void *grown;

    if (required <= *capacity) {
        return QL_STATUS_OK;
    }
    if (required > hard_limit) {
        graph->last_resource_stop = limit_reason;
        ql_error_set(error, QL_STATUS_METHOD_ERROR,
                     "e-graph %s resource limit (%zu) was reached",
                     description, hard_limit);
        return QL_STATUS_METHOD_ERROR;
    }
    next = *capacity == 0u ? 16u : *capacity;
    while (next < required) {
        if (next > hard_limit / 2u) {
            next = hard_limit;
        } else {
            next *= 2u;
        }
        if (next < required && next == hard_limit) {
            graph->last_resource_stop = limit_reason;
            ql_error_set(error, QL_STATUS_METHOD_ERROR,
                         "e-graph %s resource limit (%zu) was reached",
                         description, hard_limit);
            return QL_STATUS_METHOD_ERROR;
        }
    }
    if (next > SIZE_MAX / element_size) {
        ql_error_set(error, QL_STATUS_OUT_OF_MEMORY,
                     "e-graph %s allocation size overflowed", description);
        return QL_STATUS_OUT_OF_MEMORY;
    }
    grown = graph->allocator.reallocate(graph->allocator.user_data, *storage,
                                        next * element_size);
    if (grown == NULL) {
        ql_error_set(error, QL_STATUS_OUT_OF_MEMORY,
                     "could not grow e-graph %s storage", description);
        return QL_STATUS_OUT_OF_MEMORY;
    }
    *storage = grown;
    *capacity = next;
    return QL_STATUS_OK;
}

static size_t config_limit(uint64_t limit) {
    const uint64_t size_limit = (uint64_t)SIZE_MAX;
    return (size_t)(limit < size_limit ? limit : size_limit);
}

static uint64_t syntax_hash(ql_egraph_type type, ql_egraph_operator op,
                            const ql_egraph_term_id *operands,
                            size_t operand_count, const char *symbol,
                            const uint8_t *constant, size_t constant_size) {
    uint64_t hash = UINT64_C(1469598103934665603);
    size_t index;
    hash = hash_u32(hash, type.kind);
    hash = hash_u32(hash, type.bit_width);
    hash = hash_u32(hash, op);
    hash = hash_u32(hash, (uint32_t)operand_count);
    for (index = 0u; index < operand_count; ++index) {
        hash = hash_u32(hash, operands[index]);
    }
    if (symbol != NULL) {
        hash = hash_bytes(hash, (const uint8_t *)symbol, strlen(symbol));
    }
    if (constant != NULL) {
        hash = hash_bytes(hash, constant, constant_size);
    }
    return hash;
}

static int node_matches_syntax(const ql_egraph_node *node,
                               ql_egraph_type type,
                               ql_egraph_operator op,
                               const ql_egraph_term_id *operands,
                               size_t operand_count, const char *symbol,
                               const uint8_t *constant,
                               size_t constant_size) {
    if (!types_equal(node->type, type) || node->op != op ||
        (size_t)node->operand_count != operand_count ||
        node->constant_size != constant_size) {
        return 0;
    }
    if (operand_count != 0u &&
        memcmp(node->operands, operands,
               operand_count * sizeof(*operands)) != 0) {
        return 0;
    }
    if ((node->symbol == NULL) != (symbol == NULL) ||
        (node->constant_le == NULL) != (constant == NULL)) {
        return 0;
    }
    if (symbol != NULL && strcmp(node->symbol, symbol) != 0) {
        return 0;
    }
    return constant == NULL ||
           memcmp(node->constant_le, constant, constant_size) == 0;
}

static ql_status rehash_exact(ql_egraph *graph, size_t capacity,
                              ql_error *error) {
    ql_egraph_term_id *table;
    size_t index;

    if (capacity > SIZE_MAX / sizeof(*table)) {
        ql_error_set(error, QL_STATUS_OUT_OF_MEMORY,
                     "e-graph hash table size overflowed");
        return QL_STATUS_OUT_OF_MEMORY;
    }
    table = graph->allocator.allocate(graph->allocator.user_data,
                                      capacity * sizeof(*table));
    if (table == NULL) {
        ql_error_set(error, QL_STATUS_OUT_OF_MEMORY,
                     "could not allocate e-graph hash table");
        return QL_STATUS_OUT_OF_MEMORY;
    }
    memset(table, 0, capacity * sizeof(*table));
    for (index = 0u; index < graph->node_count; ++index) {
        const ql_egraph_node *node = &graph->nodes[index];
        const ql_egraph_term_id term = (ql_egraph_term_id)(index + 1u);
        const uint64_t hash = syntax_hash(
            node->type, node->op, node->operands, node->operand_count,
            node->symbol, node->constant_le, node->constant_size);
        size_t slot = (size_t)hash & (capacity - 1u);
        while (table[slot] != QL_EGRAPH_INVALID_TERM) {
            slot = (slot + 1u) & (capacity - 1u);
        }
        table[slot] = term;
    }
    graph->allocator.deallocate(graph->allocator.user_data,
                                graph->exact_table);
    graph->exact_table = table;
    graph->exact_capacity = capacity;
    return QL_STATUS_OK;
}

static ql_status ensure_exact_capacity(ql_egraph *graph,
                                       size_t required_terms,
                                       ql_error *error) {
    size_t capacity = graph->exact_capacity;
    if (capacity == 0u) {
        capacity = 16u;
    }
    while (required_terms > (capacity * 7u) / 10u) {
        if (capacity > SIZE_MAX / 2u) {
            ql_error_set(error, QL_STATUS_OUT_OF_MEMORY,
                         "e-graph hash table capacity overflowed");
            return QL_STATUS_OUT_OF_MEMORY;
        }
        capacity *= 2u;
    }
    if (capacity == graph->exact_capacity) {
        return QL_STATUS_OK;
    }
    return rehash_exact(graph, capacity, error);
}

static ql_egraph_term_id lookup_exact(
    const ql_egraph *graph, ql_egraph_type type, ql_egraph_operator op,
    const ql_egraph_term_id *operands, size_t operand_count,
    const char *symbol, const uint8_t *constant, size_t constant_size) {
    const uint64_t hash = syntax_hash(type, op, operands, operand_count,
                                      symbol, constant, constant_size);
    size_t slot = (size_t)hash & (graph->exact_capacity - 1u);
    for (;;) {
        const ql_egraph_term_id term = graph->exact_table[slot];
        if (term == QL_EGRAPH_INVALID_TERM) {
            return QL_EGRAPH_INVALID_TERM;
        }
        if (node_matches_syntax(&graph->nodes[(size_t)term - 1u], type, op,
                                operands, operand_count, symbol, constant,
                                constant_size)) {
            return term;
        }
        slot = (slot + 1u) & (graph->exact_capacity - 1u);
    }
}

static ql_status add_node(ql_egraph *graph, ql_egraph_type type,
                          ql_egraph_operator op,
                          const ql_egraph_term_id *operands,
                          size_t operand_count, const char *symbol,
                          const uint8_t *constant, size_t constant_size,
                          ql_egraph_term_id *out_term, ql_error *error) {
    ql_status status;
    ql_egraph_term_id existing;
    ql_egraph_node node;
    size_t slot;
    uint64_t hash;

    status = ensure_exact_capacity(graph, graph->node_count + 1u, error);
    if (status != QL_STATUS_OK) {
        return status;
    }
    existing = lookup_exact(graph, type, op, operands, operand_count, symbol,
                            constant, constant_size);
    if (existing != QL_EGRAPH_INVALID_TERM) {
        *out_term = existing;
        ql_error_clear(error);
        return QL_STATUS_OK;
    }
    if ((uint64_t)graph->node_count >= graph->config.max_terms) {
        graph->last_resource_stop = QL_EGRAPH_STOP_TERM_LIMIT;
        ql_error_set(error, QL_STATUS_METHOD_ERROR,
                     "e-graph term resource limit (%llu) was reached",
                     (unsigned long long)graph->config.max_terms);
        return QL_STATUS_METHOD_ERROR;
    }
    if ((uint64_t)graph->class_count >= graph->config.max_classes) {
        graph->last_resource_stop = QL_EGRAPH_STOP_CLASS_LIMIT;
        ql_error_set(error, QL_STATUS_METHOD_ERROR,
                     "e-graph class resource limit (%llu) was reached",
                     (unsigned long long)graph->config.max_classes);
        return QL_STATUS_METHOD_ERROR;
    }
    status = grow_vector(graph, (void **)&graph->nodes,
                         &graph->node_capacity, graph->node_count + 1u,
                         sizeof(*graph->nodes),
                         config_limit(graph->config.max_terms),
                         QL_EGRAPH_STOP_TERM_LIMIT, "term", error);
    if (status != QL_STATUS_OK) {
        return status;
    }
    status = grow_vector(graph, (void **)&graph->classes,
                         &graph->class_capacity, graph->class_count + 1u,
                         sizeof(*graph->classes),
                         config_limit(graph->config.max_classes),
                         QL_EGRAPH_STOP_CLASS_LIMIT, "class", error);
    if (status != QL_STATUS_OK) {
        return status;
    }

    memset(&node, 0, sizeof(node));
    node.type = type;
    node.op = op;
    node.operand_count = (uint32_t)operand_count;
    if (operand_count != 0u) {
        memcpy(node.operands, operands,
               operand_count * sizeof(*operands));
    }
    if (symbol != NULL) {
        const size_t length = strlen(symbol);
        node.symbol = graph->allocator.allocate(graph->allocator.user_data,
                                                 length + 1u);
        if (node.symbol == NULL) {
            ql_error_set(error, QL_STATUS_OUT_OF_MEMORY,
                         "could not copy e-graph variable symbol");
            return QL_STATUS_OUT_OF_MEMORY;
        }
        memcpy(node.symbol, symbol, length + 1u);
    }
    if (constant != NULL) {
        node.constant_le = graph->allocator.allocate(
            graph->allocator.user_data, constant_size);
        if (node.constant_le == NULL) {
            graph->allocator.deallocate(graph->allocator.user_data,
                                        node.symbol);
            ql_error_set(error, QL_STATUS_OUT_OF_MEMORY,
                         "could not copy e-graph bit-vector constant");
            return QL_STATUS_OUT_OF_MEMORY;
        }
        memcpy(node.constant_le, constant, constant_size);
        node.constant_size = constant_size;
    }
    node.initial_class = (ql_egraph_class_id)(graph->class_count + 1u);
    graph->nodes[graph->node_count] = node;
    graph->classes[graph->class_count].parent = node.initial_class;
    ++graph->node_count;
    ++graph->class_count;

    *out_term = (ql_egraph_term_id)graph->node_count;
    hash = syntax_hash(type, op, operands, operand_count, symbol, constant,
                       constant_size);
    slot = (size_t)hash & (graph->exact_capacity - 1u);
    while (graph->exact_table[slot] != QL_EGRAPH_INVALID_TERM) {
        slot = (slot + 1u) & (graph->exact_capacity - 1u);
    }
    graph->exact_table[slot] = *out_term;
    ql_error_clear(error);
    return QL_STATUS_OK;
}

static int valid_config_prefix(const ql_egraph_config_v1 *config) {
    return config->abi_version == QL_ABI_VERSION &&
           config->struct_size >= offsetof(ql_egraph_config_v1, reserved);
}

void QL_CALL ql_egraph_config_init(ql_egraph_config_v1 *config) {
    if (config == NULL) {
        return;
    }
    memset(config, 0, sizeof(*config));
    config->struct_size = sizeof(*config);
    config->abi_version = QL_ABI_VERSION;
    config->max_terms = QL_EGRAPH_DEFAULT_MAX_TERMS;
    config->max_classes = QL_EGRAPH_DEFAULT_MAX_TERMS;
    config->max_merges = QL_EGRAPH_DEFAULT_MAX_MERGES;
    config->max_bit_width = QL_EGRAPH_DEFAULT_MAX_BIT_WIDTH;
    config->max_symbol_bytes = QL_EGRAPH_DEFAULT_MAX_SYMBOL_BYTES;
}

void QL_CALL ql_egraph_saturation_limits_init(
    ql_egraph_saturation_limits_v1 *limits) {
    if (limits == NULL) {
        return;
    }
    memset(limits, 0, sizeof(*limits));
    limits->struct_size = sizeof(*limits);
    limits->abi_version = QL_ABI_VERSION;
    limits->max_iterations = QL_EGRAPH_DEFAULT_MAX_ITERATIONS;
    limits->max_rewrite_applications = QL_EGRAPH_DEFAULT_MAX_REWRITES;
}

ql_status QL_CALL ql_egraph_create(const ql_egraph_config_v1 *config,
                                    const ql_allocator *allocator,
                                    ql_egraph **out_graph,
                                    ql_error *error) {
    ql_egraph_config_v1 local_config;
    ql_egraph_config_v1 effective_config;
    const ql_allocator *selected = allocator;
    ql_egraph *graph;

    if (out_graph == NULL) {
        ql_error_set(error, QL_STATUS_INVALID_ARGUMENT,
                     "e-graph output is required");
        return QL_STATUS_INVALID_ARGUMENT;
    }
    *out_graph = NULL;
    if (config == NULL) {
        ql_egraph_config_init(&local_config);
        config = &local_config;
    }
    if (!valid_config_prefix(config)) {
        ql_error_set(error, QL_STATUS_ABI_MISMATCH,
                     "e-graph config has an incompatible ABI or size");
        return QL_STATUS_ABI_MISMATCH;
    }
    ql_egraph_config_init(&effective_config);
    memcpy(&effective_config, config,
           config->struct_size < sizeof(effective_config)
               ? config->struct_size
               : sizeof(effective_config));
    if (effective_config.max_terms == 0u ||
        effective_config.max_classes == 0u ||
        effective_config.max_merges == 0u ||
        effective_config.max_bit_width == 0u ||
        effective_config.max_symbol_bytes == 0u ||
        effective_config.max_terms > (uint64_t)UINT32_MAX - 1u ||
        effective_config.max_classes > (uint64_t)UINT32_MAX - 1u) {
        ql_error_set(error, QL_STATUS_INVALID_ARGUMENT,
                     "e-graph config contains an invalid resource limit");
        return QL_STATUS_INVALID_ARGUMENT;
    }
    if (selected == NULL) {
        selected = ql_default_allocator();
    }
    if (!ql_allocator_is_valid(selected)) {
        ql_error_set(error, QL_STATUS_INVALID_ARGUMENT,
                     "e-graph allocator is invalid");
        return QL_STATUS_INVALID_ARGUMENT;
    }
    graph = selected->allocate(selected->user_data, sizeof(*graph));
    if (graph == NULL) {
        ql_error_set(error, QL_STATUS_OUT_OF_MEMORY,
                     "could not allocate e-graph");
        return QL_STATUS_OUT_OF_MEMORY;
    }
    memset(graph, 0, sizeof(*graph));
    graph->allocator = *selected;
    graph->config = effective_config;
    *out_graph = graph;
    ql_error_clear(error);
    return QL_STATUS_OK;
}

void QL_CALL ql_egraph_destroy(ql_egraph *graph) {
    size_t index;
    if (graph == NULL) {
        return;
    }
    for (index = 0u; index < graph->node_count; ++index) {
        graph->allocator.deallocate(graph->allocator.user_data,
                                    graph->nodes[index].symbol);
        graph->allocator.deallocate(graph->allocator.user_data,
                                    graph->nodes[index].constant_le);
    }
    graph->allocator.deallocate(graph->allocator.user_data,
                                graph->exact_table);
    graph->allocator.deallocate(graph->allocator.user_data, graph->merges);
    graph->allocator.deallocate(graph->allocator.user_data, graph->classes);
    graph->allocator.deallocate(graph->allocator.user_data, graph->nodes);
    graph->allocator.deallocate(graph->allocator.user_data, graph);
}

uint32_t QL_CALL ql_egraph_operator_is_supported(ql_egraph_operator op) {
    switch (op) {
    case QL_EGRAPH_OP_VARIABLE:
    case QL_EGRAPH_OP_BOOL_CONSTANT:
    case QL_EGRAPH_OP_BV_CONSTANT:
    case QL_EGRAPH_OP_BOOL_NOT:
    case QL_EGRAPH_OP_BOOL_AND:
    case QL_EGRAPH_OP_BOOL_OR:
    case QL_EGRAPH_OP_BOOL_XOR:
    case QL_EGRAPH_OP_BV_NOT:
    case QL_EGRAPH_OP_BV_AND:
    case QL_EGRAPH_OP_BV_OR:
    case QL_EGRAPH_OP_BV_XOR:
    case QL_EGRAPH_OP_BV_ADD:
    case QL_EGRAPH_OP_BV_SUB:
    case QL_EGRAPH_OP_BV_MUL:
    case QL_EGRAPH_OP_EQUAL:
    case QL_EGRAPH_OP_ITE:
        return 1u;
    default:
        return 0u;
    }
}

static ql_status make_variable(ql_egraph *graph, const char *symbol,
                               ql_egraph_type type,
                               ql_egraph_term_id *out_term,
                               ql_error *error) {
    size_t length;
    ql_status status = require_graph(graph, error);
    if (status != QL_STATUS_OK) {
        return status;
    }
    if (out_term == NULL || symbol == NULL || symbol[0] == '\0') {
        ql_error_set(error, QL_STATUS_INVALID_ARGUMENT,
                     "a non-empty symbol and term output are required");
        return QL_STATUS_INVALID_ARGUMENT;
    }
    length = strlen(symbol);
    if (length > graph->config.max_symbol_bytes) {
        ql_error_set(error, QL_STATUS_INVALID_ARGUMENT,
                     "e-graph variable symbol exceeds %u bytes",
                     graph->config.max_symbol_bytes);
        return QL_STATUS_INVALID_ARGUMENT;
    }
    if (!valid_type(graph, type)) {
        ql_error_set(error, QL_STATUS_TYPE_MISMATCH,
                     "e-graph variable has an invalid type");
        return QL_STATUS_TYPE_MISMATCH;
    }
    return add_node(graph, type, QL_EGRAPH_OP_VARIABLE, NULL, 0u, symbol,
                    NULL, 0u, out_term, error);
}

ql_status QL_CALL ql_egraph_make_bool_variable(
    ql_egraph *graph, const char *symbol, ql_egraph_term_id *out_term,
    ql_error *error) {
    return make_variable(graph, symbol, bool_type(), out_term, error);
}

ql_status QL_CALL ql_egraph_make_bv_variable(
    ql_egraph *graph, const char *symbol, uint32_t bit_width,
    ql_egraph_term_id *out_term, ql_error *error) {
    return make_variable(graph, symbol, bv_type(bit_width), out_term, error);
}

ql_status QL_CALL ql_egraph_make_bool_constant(
    ql_egraph *graph, uint32_t value, ql_egraph_term_id *out_term,
    ql_error *error) {
    ql_status status = require_graph(graph, error);
    uint8_t byte;
    if (status != QL_STATUS_OK) {
        return status;
    }
    if (out_term == NULL || value > 1u) {
        ql_error_set(error, QL_STATUS_INVALID_ARGUMENT,
                     "Boolean constant must be zero or one and needs an output");
        return QL_STATUS_INVALID_ARGUMENT;
    }
    byte = (uint8_t)value;
    return add_node(graph, bool_type(), QL_EGRAPH_OP_BOOL_CONSTANT, NULL,
                    0u, NULL, &byte, 1u, out_term, error);
}

ql_status QL_CALL ql_egraph_make_bv_constant(
    ql_egraph *graph, uint32_t bit_width, const uint8_t *little_endian,
    size_t byte_count, ql_egraph_term_id *out_term, ql_error *error) {
    ql_status status = require_graph(graph, error);
    size_t required;
    size_t last_index;
    uint32_t used_bits;
    if (status != QL_STATUS_OK) {
        return status;
    }
    if (out_term == NULL || bit_width == 0u ||
        bit_width > graph->config.max_bit_width) {
        ql_error_set(error, QL_STATUS_TYPE_MISMATCH,
                     "bit-vector constant has an invalid width or output");
        return QL_STATUS_TYPE_MISMATCH;
    }
    last_index = (size_t)((bit_width - 1u) / 8u);
    required = last_index + 1u;
    if (little_endian == NULL || byte_count != required) {
        ql_error_set(error, QL_STATUS_INVALID_ARGUMENT,
                     "bit-vector constant requires exactly %zu little-endian bytes",
                     required);
        return QL_STATUS_INVALID_ARGUMENT;
    }
    used_bits = bit_width & 7u;
    if (used_bits != 0u &&
        (little_endian[last_index] &
         (uint8_t)~((UINT32_C(1) << used_bits) - 1u)) != 0u) {
        ql_error_set(error, QL_STATUS_INVALID_ARGUMENT,
                     "bit-vector constant has non-zero bits above its width");
        return QL_STATUS_INVALID_ARGUMENT;
    }
    return add_node(graph, bv_type(bit_width), QL_EGRAPH_OP_BV_CONSTANT,
                    NULL, 0u, NULL, little_endian, required, out_term, error);
}

ql_status QL_CALL ql_egraph_make_bv_u64(
    ql_egraph *graph, uint32_t bit_width, uint64_t value,
    ql_egraph_term_id *out_term, ql_error *error) {
    uint8_t bytes[8];
    size_t count;
    size_t index;
    if (bit_width == 0u || bit_width > 64u) {
        ql_error_set(error, QL_STATUS_INVALID_ARGUMENT,
                     "u64 convenience constants require a width from 1 to 64");
        return QL_STATUS_INVALID_ARGUMENT;
    }
    count = ((size_t)bit_width + 7u) / 8u;
    if (bit_width < 64u) {
        value &= (UINT64_C(1) << bit_width) - UINT64_C(1);
    }
    for (index = 0u; index < count; ++index) {
        bytes[index] = (uint8_t)(value & UINT64_C(255));
        value >>= 8u;
    }
    return ql_egraph_make_bv_constant(graph, bit_width, bytes, count,
                                      out_term, error);
}

static ql_status infer_operation_type(
    const ql_egraph *graph, ql_egraph_operator op,
    const ql_egraph_term_id *operands, size_t operand_count,
    ql_egraph_type *out_type, ql_error *error) {
    ql_egraph_type first;
    size_t index;
    size_t expected;

    if (!ql_egraph_operator_is_supported(op) ||
        op == QL_EGRAPH_OP_VARIABLE ||
        op == QL_EGRAPH_OP_BOOL_CONSTANT ||
        op == QL_EGRAPH_OP_BV_CONSTANT) {
        ql_error_set(error, QL_STATUS_INVALID_ARGUMENT,
                     "e-graph operator %u is unsupported by the pure term engine",
                     op);
        return QL_STATUS_INVALID_ARGUMENT;
    }
    if (op == QL_EGRAPH_OP_BOOL_NOT || op == QL_EGRAPH_OP_BV_NOT) {
        expected = 1u;
    } else if (op == QL_EGRAPH_OP_ITE) {
        expected = 3u;
    } else {
        expected = 2u;
    }
    if (operands == NULL || operand_count != expected) {
        ql_error_set(error, QL_STATUS_INVALID_ARGUMENT,
                     "e-graph operator %u requires %zu operands", op,
                     expected);
        return QL_STATUS_INVALID_ARGUMENT;
    }
    for (index = 0u; index < operand_count; ++index) {
        if (require_term(graph, operands[index], error) != QL_STATUS_OK) {
            return QL_STATUS_INVALID_ARGUMENT;
        }
    }
    first = graph->nodes[(size_t)operands[0] - 1u].type;
    switch (op) {
    case QL_EGRAPH_OP_BOOL_NOT:
    case QL_EGRAPH_OP_BOOL_AND:
    case QL_EGRAPH_OP_BOOL_OR:
    case QL_EGRAPH_OP_BOOL_XOR:
        if (first.kind != QL_EGRAPH_SORT_BOOL) {
            break;
        }
        for (index = 1u; index < operand_count; ++index) {
            if (!types_equal(first,
                             graph->nodes[(size_t)operands[index] - 1u].type)) {
                break;
            }
        }
        if (index == operand_count) {
            *out_type = bool_type();
            return QL_STATUS_OK;
        }
        break;
    case QL_EGRAPH_OP_BV_NOT:
    case QL_EGRAPH_OP_BV_AND:
    case QL_EGRAPH_OP_BV_OR:
    case QL_EGRAPH_OP_BV_XOR:
    case QL_EGRAPH_OP_BV_ADD:
    case QL_EGRAPH_OP_BV_SUB:
    case QL_EGRAPH_OP_BV_MUL:
        if (first.kind != QL_EGRAPH_SORT_BITVECTOR) {
            break;
        }
        for (index = 1u; index < operand_count; ++index) {
            if (!types_equal(first,
                             graph->nodes[(size_t)operands[index] - 1u].type)) {
                break;
            }
        }
        if (index == operand_count) {
            *out_type = first;
            return QL_STATUS_OK;
        }
        break;
    case QL_EGRAPH_OP_EQUAL:
        if (types_equal(first,
                        graph->nodes[(size_t)operands[1] - 1u].type)) {
            *out_type = bool_type();
            return QL_STATUS_OK;
        }
        break;
    case QL_EGRAPH_OP_ITE:
        if (first.kind == QL_EGRAPH_SORT_BOOL &&
            types_equal(graph->nodes[(size_t)operands[1] - 1u].type,
                        graph->nodes[(size_t)operands[2] - 1u].type)) {
            *out_type = graph->nodes[(size_t)operands[1] - 1u].type;
            return QL_STATUS_OK;
        }
        break;
    default:
        break;
    }
    ql_error_set(error, QL_STATUS_TYPE_MISMATCH,
                 "e-graph operator %u has ill-typed operands", op);
    return QL_STATUS_TYPE_MISMATCH;
}

ql_status QL_CALL ql_egraph_make_operation(
    ql_egraph *graph, ql_egraph_operator op,
    const ql_egraph_term_id *operands, size_t operand_count,
    ql_egraph_term_id *out_term, ql_error *error) {
    ql_egraph_type type;
    ql_status status = require_graph(graph, error);
    if (status != QL_STATUS_OK) {
        return status;
    }
    if (out_term == NULL) {
        ql_error_set(error, QL_STATUS_INVALID_ARGUMENT,
                     "e-graph term output is required");
        return QL_STATUS_INVALID_ARGUMENT;
    }
    status = infer_operation_type(graph, op, operands, operand_count, &type,
                                  error);
    if (status != QL_STATUS_OK) {
        return status;
    }
    return add_node(graph, type, op, operands, operand_count, NULL, NULL, 0u,
                    out_term, error);
}

static void capture_operands(ql_egraph *graph, const ql_egraph_node *node,
                             ql_egraph_class_id *classes) {
    size_t index;
    for (index = 0u; index < (size_t)node->operand_count; ++index) {
        const ql_egraph_node *operand =
            &graph->nodes[(size_t)node->operands[index] - 1u];
        classes[index] = find_root(graph, operand->initial_class);
    }
}

static ql_status merge_terms(ql_egraph *graph, ql_egraph_term_id lhs,
                             ql_egraph_term_id rhs,
                             ql_egraph_merge_kind kind,
                             const char *reason, uint32_t *did_merge,
                             ql_error *error) {
    ql_egraph_node *lhs_node = &graph->nodes[(size_t)lhs - 1u];
    ql_egraph_node *rhs_node = &graph->nodes[(size_t)rhs - 1u];
    ql_egraph_class_id lhs_root = find_root(graph, lhs_node->initial_class);
    ql_egraph_class_id rhs_root = find_root(graph, rhs_node->initial_class);
    ql_egraph_class_id parent;
    ql_egraph_class_id child;
    ql_egraph_merge_record_v1 record;
    ql_status status;

    *did_merge = 0u;
    if (lhs_root == rhs_root) {
        return QL_STATUS_OK;
    }
    if (!types_equal(lhs_node->type, rhs_node->type)) {
        ql_error_set(error, QL_STATUS_TYPE_MISMATCH,
                     "cannot merge e-graph terms with different types");
        return QL_STATUS_TYPE_MISMATCH;
    }
    if ((uint64_t)graph->merge_count >= graph->config.max_merges) {
        graph->last_resource_stop = QL_EGRAPH_STOP_MERGE_LIMIT;
        ql_error_set(error, QL_STATUS_METHOD_ERROR,
                     "e-graph merge resource limit (%llu) was reached",
                     (unsigned long long)graph->config.max_merges);
        return QL_STATUS_METHOD_ERROR;
    }
    status = grow_vector(graph, (void **)&graph->merges,
                         &graph->merge_capacity, graph->merge_count + 1u,
                         sizeof(*graph->merges),
                         config_limit(graph->config.max_merges),
                         QL_EGRAPH_STOP_MERGE_LIMIT, "merge", error);
    if (status != QL_STATUS_OK) {
        return status;
    }
    memset(&record, 0, sizeof(record));
    record.sequence = (uint64_t)graph->merge_count + 1u;
    record.kind = kind;
    record.lhs_operand_count = lhs_node->operand_count;
    record.rhs_operand_count = rhs_node->operand_count;
    record.lhs_term = lhs;
    record.rhs_term = rhs;
    record.lhs_class = lhs_root;
    record.rhs_class = rhs_root;
    capture_operands(graph, lhs_node, record.lhs_operands);
    capture_operands(graph, rhs_node, record.rhs_operands);
    memcpy(record.reason, reason, strlen(reason) + 1u);

    parent = lhs_root < rhs_root ? lhs_root : rhs_root;
    child = lhs_root < rhs_root ? rhs_root : lhs_root;
    graph->classes[(size_t)child - 1u].parent = parent;
    graph->merges[graph->merge_count++] = record;
    if (kind == QL_EGRAPH_MERGE_AXIOM) {
        ++graph->axiom_merge_count;
    }
    *did_merge = 1u;
    return QL_STATUS_OK;
}

ql_status QL_CALL ql_egraph_assume_equal(
    ql_egraph *graph, ql_egraph_term_id lhs, ql_egraph_term_id rhs,
    const char *rule_name, ql_error *error) {
    ql_status status = require_graph(graph, error);
    uint32_t merged;
    size_t length;
    if (status != QL_STATUS_OK) {
        return status;
    }
    if (require_term(graph, lhs, error) != QL_STATUS_OK ||
        require_term(graph, rhs, error) != QL_STATUS_OK) {
        return QL_STATUS_INVALID_ARGUMENT;
    }
    if (rule_name == NULL || rule_name[0] == '\0') {
        ql_error_set(error, QL_STATUS_INVALID_ARGUMENT,
                     "e-graph axiom needs a non-empty rule name");
        return QL_STATUS_INVALID_ARGUMENT;
    }
    length = strlen(rule_name);
    if (length >= QL_EGRAPH_REASON_CAPACITY) {
        ql_error_set(error, QL_STATUS_INVALID_ARGUMENT,
                     "e-graph rule name must be shorter than %u bytes",
                     QL_EGRAPH_REASON_CAPACITY);
        return QL_STATUS_INVALID_ARGUMENT;
    }
    status = merge_terms(graph, lhs, rhs, QL_EGRAPH_MERGE_AXIOM,
                         rule_name, &merged, error);
    if (status == QL_STATUS_OK) {
        ql_error_clear(error);
    }
    return status;
}

static uint64_t congruent_hash(ql_egraph *graph,
                               const ql_egraph_node *node) {
    uint64_t hash = UINT64_C(1469598103934665603);
    size_t index;
    hash = hash_u32(hash, node->type.kind);
    hash = hash_u32(hash, node->type.bit_width);
    hash = hash_u32(hash, node->op);
    hash = hash_u32(hash, node->operand_count);
    for (index = 0u; index < (size_t)node->operand_count; ++index) {
        const ql_egraph_node *operand =
            &graph->nodes[(size_t)node->operands[index] - 1u];
        hash = hash_u32(hash, find_root(graph, operand->initial_class));
    }
    if (node->symbol != NULL) {
        hash = hash_bytes(hash, (const uint8_t *)node->symbol,
                          strlen(node->symbol));
    }
    if (node->constant_le != NULL) {
        hash = hash_bytes(hash, node->constant_le, node->constant_size);
    }
    return hash;
}

static int nodes_congruent(ql_egraph *graph, const ql_egraph_node *lhs,
                           const ql_egraph_node *rhs) {
    size_t index;
    if (!types_equal(lhs->type, rhs->type) || lhs->op != rhs->op ||
        lhs->operand_count != rhs->operand_count ||
        lhs->constant_size != rhs->constant_size ||
        (lhs->symbol == NULL) != (rhs->symbol == NULL) ||
        (lhs->constant_le == NULL) != (rhs->constant_le == NULL)) {
        return 0;
    }
    if (lhs->symbol != NULL && strcmp(lhs->symbol, rhs->symbol) != 0) {
        return 0;
    }
    if (lhs->constant_le != NULL &&
        memcmp(lhs->constant_le, rhs->constant_le,
               lhs->constant_size) != 0) {
        return 0;
    }
    for (index = 0u; index < (size_t)lhs->operand_count; ++index) {
        const ql_egraph_node *lhs_operand =
            &graph->nodes[(size_t)lhs->operands[index] - 1u];
        const ql_egraph_node *rhs_operand =
            &graph->nodes[(size_t)rhs->operands[index] - 1u];
        if (find_root(graph, lhs_operand->initial_class) !=
            find_root(graph, rhs_operand->initial_class)) {
            return 0;
        }
    }
    return 1;
}

static ql_status rebuild_congruence(ql_egraph *graph, uint32_t *made_progress,
                                    ql_error *error) {
    size_t capacity = 16u;
    ql_egraph_term_id *table;
    uint32_t pass_changed;

    while (capacity < graph->node_count * 2u) {
        if (capacity > SIZE_MAX / 2u) {
            ql_error_set(error, QL_STATUS_OUT_OF_MEMORY,
                         "e-graph congruence table size overflowed");
            return QL_STATUS_OUT_OF_MEMORY;
        }
        capacity *= 2u;
    }
    if (capacity > SIZE_MAX / sizeof(*table)) {
        ql_error_set(error, QL_STATUS_OUT_OF_MEMORY,
                     "e-graph congruence table allocation overflowed");
        return QL_STATUS_OUT_OF_MEMORY;
    }
    table = graph->allocator.allocate(graph->allocator.user_data,
                                      capacity * sizeof(*table));
    if (table == NULL) {
        ql_error_set(error, QL_STATUS_OUT_OF_MEMORY,
                     "could not allocate e-graph congruence table");
        return QL_STATUS_OUT_OF_MEMORY;
    }
    do {
        size_t index;
        pass_changed = 0u;
        memset(table, 0, capacity * sizeof(*table));
        for (index = 0u; index < graph->node_count; ++index) {
            const ql_egraph_term_id term =
                (ql_egraph_term_id)(index + 1u);
            const uint64_t hash = congruent_hash(graph, &graph->nodes[index]);
            size_t slot = (size_t)hash & (capacity - 1u);
            for (;;) {
                const ql_egraph_term_id other = table[slot];
                if (other == QL_EGRAPH_INVALID_TERM) {
                    table[slot] = term;
                    break;
                }
                if (nodes_congruent(graph, &graph->nodes[index],
                                    &graph->nodes[(size_t)other - 1u])) {
                    uint32_t merged;
                    ql_status status = merge_terms(
                        graph, other, term, QL_EGRAPH_MERGE_CONGRUENCE,
                        "congruence", &merged, error);
                    if (status != QL_STATUS_OK) {
                        graph->allocator.deallocate(
                            graph->allocator.user_data, table);
                        return status;
                    }
                    if (merged != 0u) {
                        pass_changed = 1u;
                        *made_progress = 1u;
                    }
                    break;
                }
                slot = (slot + 1u) & (capacity - 1u);
            }
        }
    } while (pass_changed != 0u);
    graph->allocator.deallocate(graph->allocator.user_data, table);
    return QL_STATUS_OK;
}

static ql_egraph_term_id find_bool_constant(ql_egraph *graph,
                                             ql_egraph_class_id class_id,
                                             uint32_t value) {
    size_t index;
    const ql_egraph_class_id root = find_root(graph, class_id);
    for (index = 0u; index < graph->node_count; ++index) {
        const ql_egraph_node *node = &graph->nodes[index];
        if (node->op == QL_EGRAPH_OP_BOOL_CONSTANT &&
            node->constant_le[0] == (uint8_t)value &&
            find_root(graph, node->initial_class) == root) {
            return (ql_egraph_term_id)(index + 1u);
        }
    }
    return QL_EGRAPH_INVALID_TERM;
}

static int constant_is_zero(const ql_egraph_node *node) {
    size_t index;
    for (index = 0u; index < node->constant_size; ++index) {
        if (node->constant_le[index] != 0u) {
            return 0;
        }
    }
    return 1;
}

static int constant_is_one(const ql_egraph_node *node) {
    size_t index;
    if (node->constant_size == 0u || node->constant_le[0] != 1u) {
        return 0;
    }
    for (index = 1u; index < node->constant_size; ++index) {
        if (node->constant_le[index] != 0u) {
            return 0;
        }
    }
    return 1;
}

static int constant_is_ones(const ql_egraph_node *node) {
    size_t index;
    const uint32_t used = node->type.bit_width & 7u;
    for (index = 0u; index + 1u < node->constant_size; ++index) {
        if (node->constant_le[index] != UINT8_MAX) {
            return 0;
        }
    }
    return node->constant_size != 0u &&
           node->constant_le[node->constant_size - 1u] ==
               (used == 0u ? UINT8_MAX
                            : (uint8_t)((UINT32_C(1) << used) - 1u));
}

typedef int (*constant_predicate)(const ql_egraph_node *node);

static ql_egraph_term_id find_bv_constant(
    ql_egraph *graph, ql_egraph_class_id class_id, ql_egraph_type type,
    constant_predicate predicate) {
    size_t index;
    const ql_egraph_class_id root = find_root(graph, class_id);
    for (index = 0u; index < graph->node_count; ++index) {
        const ql_egraph_node *node = &graph->nodes[index];
        if (node->op == QL_EGRAPH_OP_BV_CONSTANT &&
            types_equal(node->type, type) && predicate(node) &&
            find_root(graph, node->initial_class) == root) {
            return (ql_egraph_term_id)(index + 1u);
        }
    }
    return QL_EGRAPH_INVALID_TERM;
}

static ql_egraph_term_id find_unary_in_class(
    ql_egraph *graph, ql_egraph_class_id class_id, ql_egraph_operator op) {
    size_t index;
    const ql_egraph_class_id root = find_root(graph, class_id);
    for (index = 0u; index < graph->node_count; ++index) {
        const ql_egraph_node *node = &graph->nodes[index];
        if (node->op == op && node->operand_count == 1u &&
            find_root(graph, node->initial_class) == root) {
            return (ql_egraph_term_id)(index + 1u);
        }
    }
    return QL_EGRAPH_INVALID_TERM;
}

static ql_status make_bv_pattern_constant(ql_egraph *graph,
                                          ql_egraph_type type,
                                          uint32_t pattern,
                                          ql_egraph_term_id *out_term,
                                          ql_error *error) {
    const size_t count = ((size_t)type.bit_width + 7u) / 8u;
    uint8_t *bytes = graph->allocator.allocate(graph->allocator.user_data,
                                               count);
    ql_status status;
    if (bytes == NULL) {
        ql_error_set(error, QL_STATUS_OUT_OF_MEMORY,
                     "could not allocate rewrite constant");
        return QL_STATUS_OUT_OF_MEMORY;
    }
    memset(bytes, pattern == 0u ? 0 : UINT8_MAX, count);
    if (pattern == 1u) {
        memset(bytes, 0, count);
        bytes[0] = 1u;
    } else if (pattern == UINT32_MAX && (type.bit_width & 7u) != 0u) {
        bytes[count - 1u] =
            (uint8_t)((UINT32_C(1) << (type.bit_width & 7u)) - 1u);
    }
    status = ql_egraph_make_bv_constant(graph, type.bit_width, bytes, count,
                                        out_term, error);
    graph->allocator.deallocate(graph->allocator.user_data, bytes);
    return status;
}

static ql_status try_rewrite(ql_egraph *graph, ql_egraph_term_id lhs,
                             ql_egraph_term_id rhs, const char *reason,
                             uint64_t max_applications,
                             uint64_t *applications, uint32_t *made_progress,
                             ql_error *error) {
    const ql_egraph_class_id lhs_root = find_root(
        graph, graph->nodes[(size_t)lhs - 1u].initial_class);
    const ql_egraph_class_id rhs_root = find_root(
        graph, graph->nodes[(size_t)rhs - 1u].initial_class);
    uint32_t merged;
    ql_status status;
    if (lhs_root == rhs_root) {
        return QL_STATUS_OK;
    }
    if (*applications >= max_applications) {
        graph->last_resource_stop = QL_EGRAPH_STOP_REWRITE_LIMIT;
        ql_error_set(error, QL_STATUS_METHOD_ERROR,
                     "e-graph rewrite application limit (%llu) was reached",
                     (unsigned long long)max_applications);
        return QL_STATUS_METHOD_ERROR;
    }
    status = merge_terms(graph, lhs, rhs, QL_EGRAPH_MERGE_REWRITE, reason,
                         &merged, error);
    if (status == QL_STATUS_OK && merged != 0u) {
        ++*applications;
        *made_progress = 1u;
    }
    return status;
}

static ql_status apply_commutativity(
    ql_egraph *graph, ql_egraph_term_id term, const char *reason,
    uint64_t max_applications, uint64_t *applications,
    uint32_t *made_progress, ql_error *error) {
    const ql_egraph_node snapshot = graph->nodes[(size_t)term - 1u];
    ql_egraph_term_id reversed_operands[2];
    ql_egraph_term_id reversed;
    size_t before;
    ql_status status;
    if (snapshot.operand_count != 2u ||
        snapshot.operands[0] == snapshot.operands[1]) {
        return QL_STATUS_OK;
    }
    reversed_operands[0] = snapshot.operands[1];
    reversed_operands[1] = snapshot.operands[0];
    before = graph->node_count;
    status = ql_egraph_make_operation(graph, snapshot.op, reversed_operands,
                                      2u, &reversed, error);
    if (status != QL_STATUS_OK) {
        return status;
    }
    if (graph->node_count != before) {
        *made_progress = 1u;
    }
    return try_rewrite(graph, term, reversed, reason, max_applications,
                       applications, made_progress, error);
}

static ql_status apply_bool_rewrites(
    ql_egraph *graph, ql_egraph_term_id term, const ql_egraph_node *node,
    uint64_t max_applications, uint64_t *applications,
    uint32_t *made_progress, ql_error *error) {
    ql_egraph_term_id true_term = QL_EGRAPH_INVALID_TERM;
    ql_egraph_term_id false_term = QL_EGRAPH_INVALID_TERM;
    ql_egraph_term_id nested;
    ql_egraph_term_id rhs;
    ql_egraph_class_id first_class;
    ql_egraph_class_id second_class;
    ql_status status;

#define QL_TRY_BOOL_REWRITE(target, name)                                  \
    do {                                                                   \
        status = try_rewrite(graph, term, (target), (name),                \
                             max_applications, applications,               \
                             made_progress, error);                        \
        if (status != QL_STATUS_OK) {                                      \
            return status;                                                 \
        }                                                                  \
    } while (0)

    first_class = find_root(
        graph, graph->nodes[(size_t)node->operands[0] - 1u].initial_class);
    if (node->op == QL_EGRAPH_OP_BOOL_NOT) {
        true_term = find_bool_constant(graph, first_class, 1u);
        false_term = find_bool_constant(graph, first_class, 0u);
        if (true_term != QL_EGRAPH_INVALID_TERM) {
            status = ql_egraph_make_bool_constant(graph, 0u, &rhs, error);
            if (status != QL_STATUS_OK) {
                return status;
            }
            QL_TRY_BOOL_REWRITE(rhs, "bool.not.true");
        }
        if (false_term != QL_EGRAPH_INVALID_TERM) {
            status = ql_egraph_make_bool_constant(graph, 1u, &rhs, error);
            if (status != QL_STATUS_OK) {
                return status;
            }
            QL_TRY_BOOL_REWRITE(rhs, "bool.not.false");
        }
        nested = find_unary_in_class(graph, first_class,
                                     QL_EGRAPH_OP_BOOL_NOT);
        if (nested != QL_EGRAPH_INVALID_TERM) {
            rhs = graph->nodes[(size_t)nested - 1u].operands[0];
            QL_TRY_BOOL_REWRITE(rhs, "bool.not.involution");
        }
        return QL_STATUS_OK;
    }

    second_class = find_root(
        graph, graph->nodes[(size_t)node->operands[1] - 1u].initial_class);
    if (first_class == second_class) {
        if (node->op == QL_EGRAPH_OP_BOOL_XOR) {
            status = ql_egraph_make_bool_constant(graph, 0u, &rhs, error);
            if (status != QL_STATUS_OK) {
                return status;
            }
            QL_TRY_BOOL_REWRITE(rhs, "bool.xor.self");
        } else {
            QL_TRY_BOOL_REWRITE(node->operands[0], "bool.idempotent");
        }
    }
    true_term = find_bool_constant(graph, first_class, 1u);
    false_term = find_bool_constant(graph, first_class, 0u);
    if (node->op == QL_EGRAPH_OP_BOOL_AND) {
        if (true_term != QL_EGRAPH_INVALID_TERM) {
            QL_TRY_BOOL_REWRITE(node->operands[1], "bool.and.true");
        }
        if (false_term != QL_EGRAPH_INVALID_TERM) {
            QL_TRY_BOOL_REWRITE(false_term, "bool.and.false");
        }
        true_term = find_bool_constant(graph, second_class, 1u);
        false_term = find_bool_constant(graph, second_class, 0u);
        if (true_term != QL_EGRAPH_INVALID_TERM) {
            QL_TRY_BOOL_REWRITE(node->operands[0], "bool.and.true");
        }
        if (false_term != QL_EGRAPH_INVALID_TERM) {
            QL_TRY_BOOL_REWRITE(false_term, "bool.and.false");
        }
        return apply_commutativity(graph, term, "bool.and.commutative",
                                   max_applications, applications,
                                   made_progress, error);
    }
    if (node->op == QL_EGRAPH_OP_BOOL_OR) {
        if (false_term != QL_EGRAPH_INVALID_TERM) {
            QL_TRY_BOOL_REWRITE(node->operands[1], "bool.or.false");
        }
        if (true_term != QL_EGRAPH_INVALID_TERM) {
            QL_TRY_BOOL_REWRITE(true_term, "bool.or.true");
        }
        true_term = find_bool_constant(graph, second_class, 1u);
        false_term = find_bool_constant(graph, second_class, 0u);
        if (false_term != QL_EGRAPH_INVALID_TERM) {
            QL_TRY_BOOL_REWRITE(node->operands[0], "bool.or.false");
        }
        if (true_term != QL_EGRAPH_INVALID_TERM) {
            QL_TRY_BOOL_REWRITE(true_term, "bool.or.true");
        }
        return apply_commutativity(graph, term, "bool.or.commutative",
                                   max_applications, applications,
                                   made_progress, error);
    }
    if (node->op == QL_EGRAPH_OP_BOOL_XOR) {
        if (false_term != QL_EGRAPH_INVALID_TERM) {
            QL_TRY_BOOL_REWRITE(node->operands[1], "bool.xor.false");
        }
        false_term = find_bool_constant(graph, second_class, 0u);
        if (false_term != QL_EGRAPH_INVALID_TERM) {
            QL_TRY_BOOL_REWRITE(node->operands[0], "bool.xor.false");
        }
        return apply_commutativity(graph, term, "bool.xor.commutative",
                                   max_applications, applications,
                                   made_progress, error);
    }
    return QL_STATUS_OK;
#undef QL_TRY_BOOL_REWRITE
}

static ql_status apply_bv_rewrites(
    ql_egraph *graph, ql_egraph_term_id term, const ql_egraph_node *node,
    uint64_t max_applications, uint64_t *applications,
    uint32_t *made_progress, ql_error *error) {
    ql_egraph_class_id first_class;
    ql_egraph_class_id second_class;
    ql_egraph_term_id zero;
    ql_egraph_term_id one;
    ql_egraph_term_id ones;
    ql_egraph_term_id nested;
    ql_egraph_term_id rhs;
    ql_status status;

#define QL_TRY_BV_REWRITE(target, name)                                    \
    do {                                                                   \
        status = try_rewrite(graph, term, (target), (name),                \
                             max_applications, applications,               \
                             made_progress, error);                        \
        if (status != QL_STATUS_OK) {                                      \
            return status;                                                 \
        }                                                                  \
    } while (0)

    first_class = find_root(
        graph, graph->nodes[(size_t)node->operands[0] - 1u].initial_class);
    if (node->op == QL_EGRAPH_OP_BV_NOT) {
        nested = find_unary_in_class(graph, first_class,
                                     QL_EGRAPH_OP_BV_NOT);
        if (nested != QL_EGRAPH_INVALID_TERM) {
            rhs = graph->nodes[(size_t)nested - 1u].operands[0];
            QL_TRY_BV_REWRITE(rhs, "bv.not.involution");
        }
        return QL_STATUS_OK;
    }
    second_class = find_root(
        graph, graph->nodes[(size_t)node->operands[1] - 1u].initial_class);
    zero = find_bv_constant(graph, first_class, node->type,
                            constant_is_zero);
    one = find_bv_constant(graph, first_class, node->type,
                           constant_is_one);
    ones = find_bv_constant(graph, first_class, node->type,
                            constant_is_ones);

    if (first_class == second_class) {
        if (node->op == QL_EGRAPH_OP_BV_XOR ||
            node->op == QL_EGRAPH_OP_BV_SUB) {
            status = make_bv_pattern_constant(graph, node->type, 0u, &rhs,
                                              error);
            if (status != QL_STATUS_OK) {
                return status;
            }
            QL_TRY_BV_REWRITE(
                rhs, node->op == QL_EGRAPH_OP_BV_XOR ? "bv.xor.self"
                                                      : "bv.sub.self");
        } else if (node->op == QL_EGRAPH_OP_BV_AND ||
                   node->op == QL_EGRAPH_OP_BV_OR) {
            QL_TRY_BV_REWRITE(node->operands[0], "bv.idempotent");
        }
    }
    if (node->op == QL_EGRAPH_OP_BV_AND) {
        if (zero != QL_EGRAPH_INVALID_TERM) {
            QL_TRY_BV_REWRITE(zero, "bv.and.zero");
        }
        if (ones != QL_EGRAPH_INVALID_TERM) {
            QL_TRY_BV_REWRITE(node->operands[1], "bv.and.ones");
        }
        zero = find_bv_constant(graph, second_class, node->type,
                                constant_is_zero);
        ones = find_bv_constant(graph, second_class, node->type,
                                constant_is_ones);
        if (zero != QL_EGRAPH_INVALID_TERM) {
            QL_TRY_BV_REWRITE(zero, "bv.and.zero");
        }
        if (ones != QL_EGRAPH_INVALID_TERM) {
            QL_TRY_BV_REWRITE(node->operands[0], "bv.and.ones");
        }
        return apply_commutativity(graph, term, "bv.and.commutative",
                                   max_applications, applications,
                                   made_progress, error);
    }
    if (node->op == QL_EGRAPH_OP_BV_OR) {
        if (zero != QL_EGRAPH_INVALID_TERM) {
            QL_TRY_BV_REWRITE(node->operands[1], "bv.or.zero");
        }
        if (ones != QL_EGRAPH_INVALID_TERM) {
            QL_TRY_BV_REWRITE(ones, "bv.or.ones");
        }
        zero = find_bv_constant(graph, second_class, node->type,
                                constant_is_zero);
        ones = find_bv_constant(graph, second_class, node->type,
                                constant_is_ones);
        if (zero != QL_EGRAPH_INVALID_TERM) {
            QL_TRY_BV_REWRITE(node->operands[0], "bv.or.zero");
        }
        if (ones != QL_EGRAPH_INVALID_TERM) {
            QL_TRY_BV_REWRITE(ones, "bv.or.ones");
        }
        return apply_commutativity(graph, term, "bv.or.commutative",
                                   max_applications, applications,
                                   made_progress, error);
    }
    if (node->op == QL_EGRAPH_OP_BV_XOR) {
        if (zero != QL_EGRAPH_INVALID_TERM) {
            QL_TRY_BV_REWRITE(node->operands[1], "bv.xor.zero");
        }
        zero = find_bv_constant(graph, second_class, node->type,
                                constant_is_zero);
        if (zero != QL_EGRAPH_INVALID_TERM) {
            QL_TRY_BV_REWRITE(node->operands[0], "bv.xor.zero");
        }
        return apply_commutativity(graph, term, "bv.xor.commutative",
                                   max_applications, applications,
                                   made_progress, error);
    }
    if (node->op == QL_EGRAPH_OP_BV_ADD) {
        if (zero != QL_EGRAPH_INVALID_TERM) {
            QL_TRY_BV_REWRITE(node->operands[1], "bv.add.zero");
        }
        zero = find_bv_constant(graph, second_class, node->type,
                                constant_is_zero);
        if (zero != QL_EGRAPH_INVALID_TERM) {
            QL_TRY_BV_REWRITE(node->operands[0], "bv.add.zero");
        }
        return apply_commutativity(graph, term, "bv.add.commutative",
                                   max_applications, applications,
                                   made_progress, error);
    }
    if (node->op == QL_EGRAPH_OP_BV_SUB) {
        zero = find_bv_constant(graph, second_class, node->type,
                                constant_is_zero);
        if (zero != QL_EGRAPH_INVALID_TERM) {
            QL_TRY_BV_REWRITE(node->operands[0], "bv.sub.zero");
        }
        return QL_STATUS_OK;
    }
    if (node->op == QL_EGRAPH_OP_BV_MUL) {
        if (zero != QL_EGRAPH_INVALID_TERM) {
            QL_TRY_BV_REWRITE(zero, "bv.mul.zero");
        }
        if (one != QL_EGRAPH_INVALID_TERM) {
            QL_TRY_BV_REWRITE(node->operands[1], "bv.mul.one");
        }
        zero = find_bv_constant(graph, second_class, node->type,
                                constant_is_zero);
        one = find_bv_constant(graph, second_class, node->type,
                               constant_is_one);
        if (zero != QL_EGRAPH_INVALID_TERM) {
            QL_TRY_BV_REWRITE(zero, "bv.mul.zero");
        }
        if (one != QL_EGRAPH_INVALID_TERM) {
            QL_TRY_BV_REWRITE(node->operands[0], "bv.mul.one");
        }
        return apply_commutativity(graph, term, "bv.mul.commutative",
                                   max_applications, applications,
                                   made_progress, error);
    }
    return QL_STATUS_OK;
#undef QL_TRY_BV_REWRITE
}

static ql_status apply_general_rewrites(
    ql_egraph *graph, ql_egraph_term_id term, const ql_egraph_node *node,
    uint64_t max_applications, uint64_t *applications,
    uint32_t *made_progress, ql_error *error) {
    ql_egraph_class_id first_class;
    ql_egraph_class_id second_class;
    ql_egraph_term_id constant;
    ql_status status;
    if (node->op == QL_EGRAPH_OP_EQUAL) {
        first_class = find_root(
            graph,
            graph->nodes[(size_t)node->operands[0] - 1u].initial_class);
        second_class = find_root(
            graph,
            graph->nodes[(size_t)node->operands[1] - 1u].initial_class);
        if (first_class == second_class) {
            status = ql_egraph_make_bool_constant(graph, 1u, &constant,
                                                  error);
            if (status != QL_STATUS_OK) {
                return status;
            }
            status = try_rewrite(graph, term, constant, "equal.reflexive",
                                 max_applications, applications,
                                 made_progress, error);
            if (status != QL_STATUS_OK) {
                return status;
            }
        }
        return apply_commutativity(graph, term, "equal.symmetric",
                                   max_applications, applications,
                                   made_progress, error);
    }
    if (node->op == QL_EGRAPH_OP_ITE) {
        const ql_egraph_node *condition =
            &graph->nodes[(size_t)node->operands[0] - 1u];
        const ql_egraph_class_id condition_class =
            find_root(graph, condition->initial_class);
        ql_egraph_term_id truth =
            find_bool_constant(graph, condition_class, 1u);
        ql_egraph_term_id falsity =
            find_bool_constant(graph, condition_class, 0u);
        const ql_egraph_class_id then_class = find_root(
            graph,
            graph->nodes[(size_t)node->operands[1] - 1u].initial_class);
        const ql_egraph_class_id else_class = find_root(
            graph,
            graph->nodes[(size_t)node->operands[2] - 1u].initial_class);
        if (truth != QL_EGRAPH_INVALID_TERM) {
            status = try_rewrite(graph, term, node->operands[1],
                                 "ite.true", max_applications, applications,
                                 made_progress, error);
            if (status != QL_STATUS_OK) {
                return status;
            }
        }
        if (falsity != QL_EGRAPH_INVALID_TERM) {
            status = try_rewrite(graph, term, node->operands[2],
                                 "ite.false", max_applications, applications,
                                 made_progress, error);
            if (status != QL_STATUS_OK) {
                return status;
            }
        }
        if (then_class == else_class) {
            return try_rewrite(graph, term, node->operands[1],
                               "ite.same", max_applications, applications,
                               made_progress, error);
        }
    }
    return QL_STATUS_OK;
}

static ql_status apply_rewrite_pass(
    ql_egraph *graph, uint64_t max_applications, uint64_t *applications,
    uint32_t *made_progress, ql_error *error) {
    const size_t snapshot_count = graph->node_count;
    size_t index;
    for (index = 0u; index < snapshot_count; ++index) {
        const ql_egraph_term_id term = (ql_egraph_term_id)(index + 1u);
        const ql_egraph_node snapshot = graph->nodes[index];
        ql_status status = QL_STATUS_OK;
        switch (snapshot.op) {
        case QL_EGRAPH_OP_BOOL_NOT:
        case QL_EGRAPH_OP_BOOL_AND:
        case QL_EGRAPH_OP_BOOL_OR:
        case QL_EGRAPH_OP_BOOL_XOR:
            status = apply_bool_rewrites(
                graph, term, &snapshot, max_applications, applications,
                made_progress, error);
            break;
        case QL_EGRAPH_OP_BV_NOT:
        case QL_EGRAPH_OP_BV_AND:
        case QL_EGRAPH_OP_BV_OR:
        case QL_EGRAPH_OP_BV_XOR:
        case QL_EGRAPH_OP_BV_ADD:
        case QL_EGRAPH_OP_BV_SUB:
        case QL_EGRAPH_OP_BV_MUL:
            status = apply_bv_rewrites(
                graph, term, &snapshot, max_applications, applications,
                made_progress, error);
            break;
        case QL_EGRAPH_OP_EQUAL:
        case QL_EGRAPH_OP_ITE:
            status = apply_general_rewrites(
                graph, term, &snapshot, max_applications, applications,
                made_progress, error);
            break;
        default:
            break;
        }
        if (status != QL_STATUS_OK) {
            return status;
        }
    }
    return QL_STATUS_OK;
}

static uint64_t live_class_count(const ql_egraph *graph) {
    size_t index;
    uint64_t count = 0u;
    for (index = 0u; index < graph->class_count; ++index) {
        const ql_egraph_class_id class_id =
            (ql_egraph_class_id)(index + 1u);
        if (find_root_const(graph, class_id) == class_id) {
            ++count;
        }
    }
    return count;
}

static void finish_saturation_result(
    const ql_egraph *graph, ql_egraph_saturation_result_v1 *result) {
    result->term_count = (uint64_t)graph->node_count;
    result->class_count = live_class_count(graph);
    result->merge_count = (uint64_t)graph->merge_count;
}

static int valid_limits_prefix(
    const ql_egraph_saturation_limits_v1 *limits) {
    return limits->abi_version == QL_ABI_VERSION &&
           limits->struct_size >=
               offsetof(ql_egraph_saturation_limits_v1, reserved);
}

ql_status QL_CALL ql_egraph_saturate(
    ql_egraph *graph, const ql_egraph_saturation_limits_v1 *limits,
    ql_egraph_saturation_result_v1 *result, ql_error *error) {
    ql_egraph_saturation_limits_v1 local_limits;
    uint32_t iteration;
    uint64_t applications = 0u;
    ql_status status;

    if (require_graph(graph, error) != QL_STATUS_OK) {
        return QL_STATUS_INVALID_ARGUMENT;
    }
    if (result == NULL) {
        ql_error_set(error, QL_STATUS_INVALID_ARGUMENT,
                     "e-graph saturation result is required");
        return QL_STATUS_INVALID_ARGUMENT;
    }
    memset(result, 0, sizeof(*result));
    if (limits == NULL) {
        ql_egraph_saturation_limits_init(&local_limits);
        limits = &local_limits;
    }
    if (!valid_limits_prefix(limits)) {
        ql_error_set(error, QL_STATUS_ABI_MISMATCH,
                     "e-graph saturation limits have an incompatible ABI or size");
        return QL_STATUS_ABI_MISMATCH;
    }
    if (limits->max_iterations == 0u ||
        limits->max_rewrite_applications == 0u) {
        ql_error_set(error, QL_STATUS_INVALID_ARGUMENT,
                     "e-graph saturation limits must be non-zero");
        return QL_STATUS_INVALID_ARGUMENT;
    }

    graph->last_resource_stop = QL_EGRAPH_STOP_SATURATED;
    for (iteration = 0u; iteration < limits->max_iterations; ++iteration) {
        uint32_t made_progress = 0u;
        status = rebuild_congruence(graph, &made_progress, error);
        if (status == QL_STATUS_OK) {
            status = apply_rewrite_pass(
                graph, limits->max_rewrite_applications, &applications,
                &made_progress, error);
        }
        if (status == QL_STATUS_OK) {
            status = rebuild_congruence(graph, &made_progress, error);
        }
        result->iterations = iteration + 1u;
        result->rewrite_applications = applications;
        if (status != QL_STATUS_OK) {
            if (status == QL_STATUS_METHOD_ERROR &&
                graph->last_resource_stop != QL_EGRAPH_STOP_SATURATED) {
                result->complete = 0u;
                result->stop_reason = graph->last_resource_stop;
                finish_saturation_result(graph, result);
                ql_error_clear(error);
                return QL_STATUS_OK;
            }
            return status;
        }
        if (made_progress == 0u) {
            result->complete = 1u;
            result->stop_reason = QL_EGRAPH_STOP_SATURATED;
            finish_saturation_result(graph, result);
            ql_error_clear(error);
            return QL_STATUS_OK;
        }
    }
    result->complete = 0u;
    result->stop_reason = QL_EGRAPH_STOP_ITERATION_LIMIT;
    result->rewrite_applications = applications;
    finish_saturation_result(graph, result);
    ql_error_clear(error);
    return QL_STATUS_OK;
}

ql_status QL_CALL ql_egraph_prove_equal(
    ql_egraph *graph, ql_egraph_term_id lhs, ql_egraph_term_id rhs,
    const ql_egraph_saturation_limits_v1 *limits,
    ql_egraph_proof_result_v1 *result, ql_error *error) {
    ql_status status;
    if (require_graph(graph, error) != QL_STATUS_OK) {
        return QL_STATUS_INVALID_ARGUMENT;
    }
    if (result == NULL) {
        ql_error_set(error, QL_STATUS_INVALID_ARGUMENT,
                     "e-graph proof result is required");
        return QL_STATUS_INVALID_ARGUMENT;
    }
    memset(result, 0, sizeof(*result));
    if (require_term(graph, lhs, error) != QL_STATUS_OK ||
        require_term(graph, rhs, error) != QL_STATUS_OK) {
        return QL_STATUS_INVALID_ARGUMENT;
    }
    if (!types_equal(graph->nodes[(size_t)lhs - 1u].type,
                     graph->nodes[(size_t)rhs - 1u].type)) {
        ql_error_set(error, QL_STATUS_TYPE_MISMATCH,
                     "cannot compare e-graph roots with different types");
        return QL_STATUS_TYPE_MISMATCH;
    }
    status = ql_egraph_saturate(graph, limits, &result->saturation, error);
    if (status != QL_STATUS_OK) {
        return status;
    }
    if (result->saturation.complete != 0u &&
        find_root(graph, graph->nodes[(size_t)lhs - 1u].initial_class) ==
            find_root(graph,
                      graph->nodes[(size_t)rhs - 1u].initial_class)) {
        result->verdict = QL_EGRAPH_VERDICT_PROVED_EQUAL;
        result->depends_on_axioms = graph->axiom_merge_count != 0u;
    } else {
        result->verdict = QL_EGRAPH_VERDICT_UNKNOWN;
    }
    ql_error_clear(error);
    return QL_STATUS_OK;
}

static int compare_nodes(const ql_egraph_node *lhs,
                         ql_egraph_term_id lhs_term,
                         const ql_egraph_node *rhs,
                         ql_egraph_term_id rhs_term) {
    size_t index;
    int comparison;
    if (lhs->op != rhs->op) {
        return lhs->op < rhs->op ? -1 : 1;
    }
    if (lhs->type.kind != rhs->type.kind) {
        return lhs->type.kind < rhs->type.kind ? -1 : 1;
    }
    if (lhs->type.bit_width != rhs->type.bit_width) {
        return lhs->type.bit_width < rhs->type.bit_width ? -1 : 1;
    }
    if (lhs->symbol != NULL || rhs->symbol != NULL) {
        if (lhs->symbol == NULL) {
            return -1;
        }
        if (rhs->symbol == NULL) {
            return 1;
        }
        comparison = strcmp(lhs->symbol, rhs->symbol);
        if (comparison != 0) {
            return comparison;
        }
    }
    if (lhs->constant_size != rhs->constant_size) {
        return lhs->constant_size < rhs->constant_size ? -1 : 1;
    }
    if (lhs->constant_size != 0u) {
        comparison = memcmp(lhs->constant_le, rhs->constant_le,
                            lhs->constant_size);
        if (comparison != 0) {
            return comparison;
        }
    }
    if (lhs->operand_count != rhs->operand_count) {
        return lhs->operand_count < rhs->operand_count ? -1 : 1;
    }
    for (index = 0u; index < (size_t)lhs->operand_count; ++index) {
        if (lhs->operands[index] != rhs->operands[index]) {
            return lhs->operands[index] < rhs->operands[index] ? -1 : 1;
        }
    }
    if (lhs_term == rhs_term) {
        return 0;
    }
    return lhs_term < rhs_term ? -1 : 1;
}

ql_status QL_CALL ql_egraph_extract(
    const ql_egraph *graph, ql_egraph_term_id root_term,
    ql_egraph_term_id *out_term, uint64_t *out_cost, ql_error *error) {
    uint64_t *costs;
    ql_egraph_term_id *best;
    size_t bytes;
    size_t iteration;
    ql_egraph_class_id wanted;
    const uint64_t infinity = UINT64_MAX;

    if (require_graph(graph, error) != QL_STATUS_OK) {
        return QL_STATUS_INVALID_ARGUMENT;
    }
    if (require_term(graph, root_term, error) != QL_STATUS_OK) {
        return QL_STATUS_INVALID_ARGUMENT;
    }
    if (out_term == NULL || out_cost == NULL) {
        ql_error_set(error, QL_STATUS_INVALID_ARGUMENT,
                     "e-graph extraction needs term and cost outputs");
        return QL_STATUS_INVALID_ARGUMENT;
    }
    if (graph->class_count > SIZE_MAX / sizeof(*costs)) {
        ql_error_set(error, QL_STATUS_OUT_OF_MEMORY,
                     "e-graph extraction allocation overflowed");
        return QL_STATUS_OUT_OF_MEMORY;
    }
    bytes = graph->class_count * sizeof(*costs);
    costs = graph->allocator.allocate(graph->allocator.user_data, bytes);
    best = graph->allocator.allocate(graph->allocator.user_data,
                                     graph->class_count * sizeof(*best));
    if (costs == NULL || best == NULL) {
        graph->allocator.deallocate(graph->allocator.user_data, costs);
        graph->allocator.deallocate(graph->allocator.user_data, best);
        ql_error_set(error, QL_STATUS_OUT_OF_MEMORY,
                     "could not allocate e-graph extraction workspace");
        return QL_STATUS_OUT_OF_MEMORY;
    }
    for (iteration = 0u; iteration < graph->class_count; ++iteration) {
        costs[iteration] = infinity;
        best[iteration] = QL_EGRAPH_INVALID_TERM;
    }
    for (iteration = 0u; iteration <= graph->class_count; ++iteration) {
        size_t node_index;
        uint32_t changed = 0u;
        for (node_index = 0u; node_index < graph->node_count; ++node_index) {
            const ql_egraph_node *node = &graph->nodes[node_index];
            const ql_egraph_term_id term =
                (ql_egraph_term_id)(node_index + 1u);
            const ql_egraph_class_id root =
                find_root_const(graph, node->initial_class);
            uint64_t candidate = 1u;
            size_t operand_index;
            for (operand_index = 0u;
                 operand_index < (size_t)node->operand_count;
                 ++operand_index) {
                const ql_egraph_node *operand =
                    &graph->nodes[(size_t)node->operands[operand_index] - 1u];
                const ql_egraph_class_id operand_root =
                    find_root_const(graph, operand->initial_class);
                const uint64_t operand_cost =
                    costs[(size_t)operand_root - 1u];
                if (operand_cost == infinity ||
                    candidate > UINT64_MAX - operand_cost) {
                    candidate = infinity;
                    break;
                }
                candidate += operand_cost;
            }
            if (candidate == infinity) {
                continue;
            }
            if (candidate < costs[(size_t)root - 1u] ||
                (candidate == costs[(size_t)root - 1u] &&
                 (best[(size_t)root - 1u] == QL_EGRAPH_INVALID_TERM ||
                  compare_nodes(
                      node, term,
                      &graph->nodes[(size_t)best[(size_t)root - 1u] - 1u],
                      best[(size_t)root - 1u]) < 0))) {
                costs[(size_t)root - 1u] = candidate;
                best[(size_t)root - 1u] = term;
                changed = 1u;
            }
        }
        if (changed == 0u) {
            break;
        }
    }
    wanted = find_root_const(
        graph, graph->nodes[(size_t)root_term - 1u].initial_class);
    if (best[(size_t)wanted - 1u] == QL_EGRAPH_INVALID_TERM) {
        graph->allocator.deallocate(graph->allocator.user_data, costs);
        graph->allocator.deallocate(graph->allocator.user_data, best);
        ql_error_set(error, QL_STATUS_INTERNAL_ERROR,
                     "e-graph class has no finite representative");
        return QL_STATUS_INTERNAL_ERROR;
    }
    *out_term = best[(size_t)wanted - 1u];
    *out_cost = costs[(size_t)wanted - 1u];
    graph->allocator.deallocate(graph->allocator.user_data, costs);
    graph->allocator.deallocate(graph->allocator.user_data, best);
    ql_error_clear(error);
    return QL_STATUS_OK;
}

ql_status QL_CALL ql_egraph_get_term(const ql_egraph *graph,
                                     ql_egraph_term_id term,
                                     ql_egraph_term_view_v1 *view,
                                     ql_error *error) {
    const ql_egraph_node *node;
    if (require_graph(graph, error) != QL_STATUS_OK) {
        return QL_STATUS_INVALID_ARGUMENT;
    }
    if (require_term(graph, term, error) != QL_STATUS_OK) {
        return QL_STATUS_INVALID_ARGUMENT;
    }
    if (view == NULL) {
        ql_error_set(error, QL_STATUS_INVALID_ARGUMENT,
                     "e-graph term view is required");
        return QL_STATUS_INVALID_ARGUMENT;
    }
    node = &graph->nodes[(size_t)term - 1u];
    memset(view, 0, sizeof(*view));
    view->term = term;
    view->current_class = find_root_const(graph, node->initial_class);
    view->type = node->type;
    view->op = node->op;
    view->operand_count = node->operand_count;
    memcpy(view->operands, node->operands, sizeof(view->operands));
    view->symbol = node->symbol;
    view->constant_le = node->constant_le;
    view->constant_size = node->constant_size;
    ql_error_clear(error);
    return QL_STATUS_OK;
}

ql_status QL_CALL ql_egraph_evidence(
    const ql_egraph *graph, ql_egraph_evidence_view_v1 *view,
    ql_error *error) {
    if (require_graph(graph, error) != QL_STATUS_OK) {
        return QL_STATUS_INVALID_ARGUMENT;
    }
    if (view == NULL) {
        ql_error_set(error, QL_STATUS_INVALID_ARGUMENT,
                     "e-graph evidence view is required");
        return QL_STATUS_INVALID_ARGUMENT;
    }
    view->records = graph->merges;
    view->count = graph->merge_count;
    ql_error_clear(error);
    return QL_STATUS_OK;
}

uint64_t QL_CALL ql_egraph_term_count(const ql_egraph *graph) {
    return graph == NULL ? 0u : (uint64_t)graph->node_count;
}

uint64_t QL_CALL ql_egraph_class_count(const ql_egraph *graph) {
    return graph == NULL ? 0u : live_class_count(graph);
}

uint64_t QL_CALL ql_egraph_merge_count(const ql_egraph *graph) {
    return graph == NULL ? 0u : (uint64_t)graph->merge_count;
}

ql_status QL_CALL ql_egraph_term_initial_class(
    const ql_egraph *graph, ql_egraph_term_id term,
    ql_egraph_class_id *out_class, ql_error *error) {
    if (require_graph(graph, error) != QL_STATUS_OK) {
        return QL_STATUS_INVALID_ARGUMENT;
    }
    if (require_term(graph, term, error) != QL_STATUS_OK) {
        return QL_STATUS_INVALID_ARGUMENT;
    }
    if (out_class == NULL) {
        ql_error_set(error, QL_STATUS_INVALID_ARGUMENT,
                     "e-graph initial class output is required");
        return QL_STATUS_INVALID_ARGUMENT;
    }
    *out_class = graph->nodes[(size_t)term - 1u].initial_class;
    ql_error_clear(error);
    return QL_STATUS_OK;
}

/* --- Rewrite rule catalogue -------------------------------------------- */

/* The premises below are stated for the pure term semantics this engine
   implements: bit-vector arithmetic is total modulo 2^width and there is no
   poison, trap, or effect ordering. QL_EGRAPH_RULE_COND_TOTAL_ARITHMETIC
   marks the rules that rely on that totality, so a frontend whose source
   language makes the same operation undefined on overflow can see, from the
   evidence alone, which merges it still owes an argument for. */

#define QL_RULE_ST QL_EGRAPH_RULE_COND_SAME_TYPE
#define QL_RULE_OS QL_EGRAPH_RULE_COND_OPERAND_SORT
#define QL_RULE_EQ QL_EGRAPH_RULE_COND_OPERANDS_SAME_CLASS
#define QL_RULE_WC QL_EGRAPH_RULE_COND_WITNESS_CONSTANT
#define QL_RULE_NA QL_EGRAPH_RULE_COND_NESTED_APPLICATION
#define QL_RULE_TA QL_EGRAPH_RULE_COND_TOTAL_ARITHMETIC
#define QL_RULE_SA QL_EGRAPH_RULE_COND_SIGN_AGNOSTIC
#define QL_RULE_WA QL_EGRAPH_RULE_COND_WIDTH_AGNOSTIC
#define QL_RULE_BOOL_BASE (QL_RULE_ST | QL_RULE_OS)
#define QL_RULE_BV_BASE (QL_RULE_ST | QL_RULE_OS | QL_RULE_SA | QL_RULE_WA)
#define QL_RULE_NONE QL_EGRAPH_RULE_CONSTANT_NONE
#define QL_RULE_OP_NONE QL_EGRAPH_RULE_OPERAND_NONE
#define QL_RULE_OP_ANY QL_EGRAPH_RULE_OPERAND_ANY
#define QL_RULE_OP_OTHER QL_EGRAPH_RULE_OPERAND_OTHER

typedef struct ql_egraph_rule_entry {
    const char *name;
    const char *soundness;
    ql_egraph_rule_shape shape;
    uint32_t op_count;
    ql_egraph_operator ops[QL_EGRAPH_RULE_MAX_SUBJECT_OPS];
    uint32_t arity;
    ql_egraph_rule_conditions conditions;
    ql_egraph_rule_constant witness;
    ql_egraph_rule_constant result;
    uint32_t witness_operand;
    uint32_t result_operand;
    uint32_t equal_operands[2];
    uint32_t minimum_bit_width;
    uint32_t maximum_bit_width;
} ql_egraph_rule_entry;

static const ql_egraph_rule_entry ql_egraph_rules[] = {
    {"bool.not.true",
     "not(true) = false in two-valued propositional logic",
     QL_EGRAPH_RULE_SHAPE_CONSTANT_FOLD, 1u,
     {QL_EGRAPH_OP_BOOL_NOT, 0u, 0u, 0u}, 1u,
     QL_RULE_BOOL_BASE | QL_RULE_WC, QL_EGRAPH_RULE_CONSTANT_BOOL_TRUE,
     QL_EGRAPH_RULE_CONSTANT_BOOL_FALSE, 0u, QL_RULE_OP_NONE, {0u, 0u}, 1u,
     1u},
    {"bool.not.false",
     "not(false) = true in two-valued propositional logic",
     QL_EGRAPH_RULE_SHAPE_CONSTANT_FOLD, 1u,
     {QL_EGRAPH_OP_BOOL_NOT, 0u, 0u, 0u}, 1u,
     QL_RULE_BOOL_BASE | QL_RULE_WC, QL_EGRAPH_RULE_CONSTANT_BOOL_FALSE,
     QL_EGRAPH_RULE_CONSTANT_BOOL_TRUE, 0u, QL_RULE_OP_NONE, {0u, 0u}, 1u,
     1u},
    {"bool.not.involution", "not(not(a)) = a",
     QL_EGRAPH_RULE_SHAPE_INVOLUTION, 1u,
     {QL_EGRAPH_OP_BOOL_NOT, 0u, 0u, 0u}, 1u,
     QL_RULE_BOOL_BASE | QL_RULE_NA, QL_RULE_NONE, QL_RULE_NONE, 0u,
     QL_RULE_OP_NONE, {0u, 0u}, 1u, 1u},
    {"bool.idempotent",
     "and(a, a) = a and or(a, a) = a; both operands must already be in one "
     "class",
     QL_EGRAPH_RULE_SHAPE_IDEMPOTENT, 2u,
     {QL_EGRAPH_OP_BOOL_AND, QL_EGRAPH_OP_BOOL_OR, 0u, 0u}, 2u,
     QL_RULE_BOOL_BASE | QL_RULE_EQ, QL_RULE_NONE, QL_RULE_NONE,
     QL_RULE_OP_NONE, 0u, {0u, 1u}, 1u, 1u},
    {"bool.xor.self", "xor(a, a) = false",
     QL_EGRAPH_RULE_SHAPE_SELF_ANNIHILATION, 1u,
     {QL_EGRAPH_OP_BOOL_XOR, 0u, 0u, 0u}, 2u, QL_RULE_BOOL_BASE | QL_RULE_EQ,
     QL_RULE_NONE, QL_EGRAPH_RULE_CONSTANT_BOOL_FALSE, QL_RULE_OP_NONE,
     QL_RULE_OP_NONE, {0u, 1u}, 1u, 1u},
    {"bool.and.true", "and(a, true) = a; true is the identity of and",
     QL_EGRAPH_RULE_SHAPE_IDENTITY_ELEMENT, 1u,
     {QL_EGRAPH_OP_BOOL_AND, 0u, 0u, 0u}, 2u, QL_RULE_BOOL_BASE | QL_RULE_WC,
     QL_EGRAPH_RULE_CONSTANT_BOOL_TRUE, QL_RULE_NONE, QL_RULE_OP_ANY,
     QL_RULE_OP_OTHER, {0u, 0u}, 1u, 1u},
    {"bool.and.false", "and(a, false) = false; false absorbs and",
     QL_EGRAPH_RULE_SHAPE_ABSORBING_ELEMENT, 1u,
     {QL_EGRAPH_OP_BOOL_AND, 0u, 0u, 0u}, 2u, QL_RULE_BOOL_BASE | QL_RULE_WC,
     QL_EGRAPH_RULE_CONSTANT_BOOL_FALSE,
     QL_EGRAPH_RULE_CONSTANT_BOOL_FALSE, QL_RULE_OP_ANY, QL_RULE_OP_NONE,
     {0u, 0u}, 1u, 1u},
    {"bool.and.commutative", "and(a, b) = and(b, a)",
     QL_EGRAPH_RULE_SHAPE_COMMUTATIVE, 1u,
     {QL_EGRAPH_OP_BOOL_AND, 0u, 0u, 0u}, 2u, QL_RULE_BOOL_BASE,
     QL_RULE_NONE, QL_RULE_NONE, QL_RULE_OP_NONE, QL_RULE_OP_NONE, {0u, 0u},
     1u, 1u},
    {"bool.or.false", "or(a, false) = a; false is the identity of or",
     QL_EGRAPH_RULE_SHAPE_IDENTITY_ELEMENT, 1u,
     {QL_EGRAPH_OP_BOOL_OR, 0u, 0u, 0u}, 2u, QL_RULE_BOOL_BASE | QL_RULE_WC,
     QL_EGRAPH_RULE_CONSTANT_BOOL_FALSE, QL_RULE_NONE, QL_RULE_OP_ANY,
     QL_RULE_OP_OTHER, {0u, 0u}, 1u, 1u},
    {"bool.or.true", "or(a, true) = true; true absorbs or",
     QL_EGRAPH_RULE_SHAPE_ABSORBING_ELEMENT, 1u,
     {QL_EGRAPH_OP_BOOL_OR, 0u, 0u, 0u}, 2u, QL_RULE_BOOL_BASE | QL_RULE_WC,
     QL_EGRAPH_RULE_CONSTANT_BOOL_TRUE, QL_EGRAPH_RULE_CONSTANT_BOOL_TRUE,
     QL_RULE_OP_ANY, QL_RULE_OP_NONE, {0u, 0u}, 1u, 1u},
    {"bool.or.commutative", "or(a, b) = or(b, a)",
     QL_EGRAPH_RULE_SHAPE_COMMUTATIVE, 1u,
     {QL_EGRAPH_OP_BOOL_OR, 0u, 0u, 0u}, 2u, QL_RULE_BOOL_BASE,
     QL_RULE_NONE, QL_RULE_NONE, QL_RULE_OP_NONE, QL_RULE_OP_NONE, {0u, 0u},
     1u, 1u},
    {"bool.xor.false", "xor(a, false) = a; false is the identity of xor",
     QL_EGRAPH_RULE_SHAPE_IDENTITY_ELEMENT, 1u,
     {QL_EGRAPH_OP_BOOL_XOR, 0u, 0u, 0u}, 2u, QL_RULE_BOOL_BASE | QL_RULE_WC,
     QL_EGRAPH_RULE_CONSTANT_BOOL_FALSE, QL_RULE_NONE, QL_RULE_OP_ANY,
     QL_RULE_OP_OTHER, {0u, 0u}, 1u, 1u},
    {"bool.xor.commutative", "xor(a, b) = xor(b, a)",
     QL_EGRAPH_RULE_SHAPE_COMMUTATIVE, 1u,
     {QL_EGRAPH_OP_BOOL_XOR, 0u, 0u, 0u}, 2u, QL_RULE_BOOL_BASE,
     QL_RULE_NONE, QL_RULE_NONE, QL_RULE_OP_NONE, QL_RULE_OP_NONE, {0u, 0u},
     1u, 1u},

    {"bv.not.involution", "bvnot(bvnot(a)) = a at every width",
     QL_EGRAPH_RULE_SHAPE_INVOLUTION, 1u, {QL_EGRAPH_OP_BV_NOT, 0u, 0u, 0u},
     1u, QL_RULE_BV_BASE | QL_RULE_NA, QL_RULE_NONE, QL_RULE_NONE, 0u,
     QL_RULE_OP_NONE, {0u, 0u}, 1u, 0u},
    {"bv.idempotent", "bvand(a, a) = a and bvor(a, a) = a, bitwise at "
     "every width",
     QL_EGRAPH_RULE_SHAPE_IDEMPOTENT, 2u,
     {QL_EGRAPH_OP_BV_AND, QL_EGRAPH_OP_BV_OR, 0u, 0u}, 2u,
     QL_RULE_BV_BASE | QL_RULE_EQ, QL_RULE_NONE, QL_RULE_NONE,
     QL_RULE_OP_NONE, 0u, {0u, 1u}, 1u, 0u},
    {"bv.xor.self", "bvxor(a, a) = 0 bitwise at every width",
     QL_EGRAPH_RULE_SHAPE_SELF_ANNIHILATION, 1u,
     {QL_EGRAPH_OP_BV_XOR, 0u, 0u, 0u}, 2u, QL_RULE_BV_BASE | QL_RULE_EQ,
     QL_RULE_NONE, QL_EGRAPH_RULE_CONSTANT_BV_ZERO, QL_RULE_OP_NONE,
     QL_RULE_OP_NONE, {0u, 1u}, 1u, 0u},
    {"bv.sub.self",
     "bvsub(a, a) = 0 under total arithmetic modulo 2^width; a source "
     "language whose subtraction may trap or be undefined must discharge "
     "that separately",
     QL_EGRAPH_RULE_SHAPE_SELF_ANNIHILATION, 1u,
     {QL_EGRAPH_OP_BV_SUB, 0u, 0u, 0u}, 2u,
     QL_RULE_BV_BASE | QL_RULE_EQ | QL_RULE_TA, QL_RULE_NONE,
     QL_EGRAPH_RULE_CONSTANT_BV_ZERO, QL_RULE_OP_NONE, QL_RULE_OP_NONE,
     {0u, 1u}, 1u, 0u},
    {"bv.and.zero", "bvand(a, 0) = 0 bitwise at every width",
     QL_EGRAPH_RULE_SHAPE_ABSORBING_ELEMENT, 1u,
     {QL_EGRAPH_OP_BV_AND, 0u, 0u, 0u}, 2u, QL_RULE_BV_BASE | QL_RULE_WC,
     QL_EGRAPH_RULE_CONSTANT_BV_ZERO, QL_EGRAPH_RULE_CONSTANT_BV_ZERO,
     QL_RULE_OP_ANY, QL_RULE_OP_NONE, {0u, 0u}, 1u, 0u},
    {"bv.and.ones",
     "bvand(a, ~0) = a; the all-ones constant is width-dependent and its "
     "high bits must be exactly the width, not a wider pattern",
     QL_EGRAPH_RULE_SHAPE_IDENTITY_ELEMENT, 1u,
     {QL_EGRAPH_OP_BV_AND, 0u, 0u, 0u}, 2u, QL_RULE_BV_BASE | QL_RULE_WC,
     QL_EGRAPH_RULE_CONSTANT_BV_ONES, QL_RULE_NONE, QL_RULE_OP_ANY,
     QL_RULE_OP_OTHER, {0u, 0u}, 1u, 0u},
    {"bv.and.commutative", "bvand(a, b) = bvand(b, a)",
     QL_EGRAPH_RULE_SHAPE_COMMUTATIVE, 1u, {QL_EGRAPH_OP_BV_AND, 0u, 0u, 0u},
     2u, QL_RULE_BV_BASE, QL_RULE_NONE, QL_RULE_NONE, QL_RULE_OP_NONE,
     QL_RULE_OP_NONE, {0u, 0u}, 1u, 0u},
    {"bv.or.zero", "bvor(a, 0) = a bitwise at every width",
     QL_EGRAPH_RULE_SHAPE_IDENTITY_ELEMENT, 1u,
     {QL_EGRAPH_OP_BV_OR, 0u, 0u, 0u}, 2u, QL_RULE_BV_BASE | QL_RULE_WC,
     QL_EGRAPH_RULE_CONSTANT_BV_ZERO, QL_RULE_NONE, QL_RULE_OP_ANY,
     QL_RULE_OP_OTHER, {0u, 0u}, 1u, 0u},
    {"bv.or.ones", "bvor(a, ~0) = ~0 at the subject width",
     QL_EGRAPH_RULE_SHAPE_ABSORBING_ELEMENT, 1u,
     {QL_EGRAPH_OP_BV_OR, 0u, 0u, 0u}, 2u, QL_RULE_BV_BASE | QL_RULE_WC,
     QL_EGRAPH_RULE_CONSTANT_BV_ONES, QL_EGRAPH_RULE_CONSTANT_BV_ONES,
     QL_RULE_OP_ANY, QL_RULE_OP_NONE, {0u, 0u}, 1u, 0u},
    {"bv.or.commutative", "bvor(a, b) = bvor(b, a)",
     QL_EGRAPH_RULE_SHAPE_COMMUTATIVE, 1u, {QL_EGRAPH_OP_BV_OR, 0u, 0u, 0u},
     2u, QL_RULE_BV_BASE, QL_RULE_NONE, QL_RULE_NONE, QL_RULE_OP_NONE,
     QL_RULE_OP_NONE, {0u, 0u}, 1u, 0u},
    {"bv.xor.zero", "bvxor(a, 0) = a bitwise at every width",
     QL_EGRAPH_RULE_SHAPE_IDENTITY_ELEMENT, 1u,
     {QL_EGRAPH_OP_BV_XOR, 0u, 0u, 0u}, 2u, QL_RULE_BV_BASE | QL_RULE_WC,
     QL_EGRAPH_RULE_CONSTANT_BV_ZERO, QL_RULE_NONE, QL_RULE_OP_ANY,
     QL_RULE_OP_OTHER, {0u, 0u}, 1u, 0u},
    {"bv.xor.commutative", "bvxor(a, b) = bvxor(b, a)",
     QL_EGRAPH_RULE_SHAPE_COMMUTATIVE, 1u, {QL_EGRAPH_OP_BV_XOR, 0u, 0u, 0u},
     2u, QL_RULE_BV_BASE, QL_RULE_NONE, QL_RULE_NONE, QL_RULE_OP_NONE,
     QL_RULE_OP_NONE, {0u, 0u}, 1u, 0u},
    {"bv.add.zero",
     "bvadd(a, 0) = a under total arithmetic modulo 2^width; the identity "
     "does not depend on the signed or unsigned reading",
     QL_EGRAPH_RULE_SHAPE_IDENTITY_ELEMENT, 1u,
     {QL_EGRAPH_OP_BV_ADD, 0u, 0u, 0u}, 2u,
     QL_RULE_BV_BASE | QL_RULE_WC | QL_RULE_TA,
     QL_EGRAPH_RULE_CONSTANT_BV_ZERO, QL_RULE_NONE, QL_RULE_OP_ANY,
     QL_RULE_OP_OTHER, {0u, 0u}, 1u, 0u},
    {"bv.add.commutative",
     "bvadd(a, b) = bvadd(b, a) under total arithmetic modulo 2^width",
     QL_EGRAPH_RULE_SHAPE_COMMUTATIVE, 1u, {QL_EGRAPH_OP_BV_ADD, 0u, 0u, 0u},
     2u, QL_RULE_BV_BASE | QL_RULE_TA, QL_RULE_NONE, QL_RULE_NONE,
     QL_RULE_OP_NONE, QL_RULE_OP_NONE, {0u, 0u}, 1u, 0u},
    {"bv.sub.zero",
     "bvsub(a, 0) = a under total arithmetic modulo 2^width; the zero must "
     "be the right operand because subtraction is not commutative",
     QL_EGRAPH_RULE_SHAPE_IDENTITY_ELEMENT, 1u,
     {QL_EGRAPH_OP_BV_SUB, 0u, 0u, 0u}, 2u,
     QL_RULE_BV_BASE | QL_RULE_WC | QL_RULE_TA,
     QL_EGRAPH_RULE_CONSTANT_BV_ZERO, QL_RULE_NONE, 1u, 0u, {0u, 0u}, 1u,
     0u},
    {"bv.mul.zero",
     "bvmul(a, 0) = 0 under total arithmetic modulo 2^width",
     QL_EGRAPH_RULE_SHAPE_ABSORBING_ELEMENT, 1u,
     {QL_EGRAPH_OP_BV_MUL, 0u, 0u, 0u}, 2u,
     QL_RULE_BV_BASE | QL_RULE_WC | QL_RULE_TA,
     QL_EGRAPH_RULE_CONSTANT_BV_ZERO, QL_EGRAPH_RULE_CONSTANT_BV_ZERO,
     QL_RULE_OP_ANY, QL_RULE_OP_NONE, {0u, 0u}, 1u, 0u},
    {"bv.mul.one",
     "bvmul(a, 1) = a under total arithmetic modulo 2^width; the one "
     "constant is the width-correct unit, not a wider literal",
     QL_EGRAPH_RULE_SHAPE_IDENTITY_ELEMENT, 1u,
     {QL_EGRAPH_OP_BV_MUL, 0u, 0u, 0u}, 2u,
     QL_RULE_BV_BASE | QL_RULE_WC | QL_RULE_TA,
     QL_EGRAPH_RULE_CONSTANT_BV_ONE, QL_RULE_NONE, QL_RULE_OP_ANY,
     QL_RULE_OP_OTHER, {0u, 0u}, 1u, 0u},
    {"bv.mul.commutative",
     "bvmul(a, b) = bvmul(b, a) under total arithmetic modulo 2^width",
     QL_EGRAPH_RULE_SHAPE_COMMUTATIVE, 1u, {QL_EGRAPH_OP_BV_MUL, 0u, 0u, 0u},
     2u, QL_RULE_BV_BASE | QL_RULE_TA, QL_RULE_NONE, QL_RULE_NONE,
     QL_RULE_OP_NONE, QL_RULE_OP_NONE, {0u, 0u}, 1u, 0u},

    {"equal.reflexive",
     "equal(a, a) = true; the operands must already share a class, and the "
     "subject sort is bool while the operand sort is not constrained by it",
     QL_EGRAPH_RULE_SHAPE_REFLEXIVE, 1u, {QL_EGRAPH_OP_EQUAL, 0u, 0u, 0u},
     2u, QL_RULE_ST | QL_RULE_EQ, QL_RULE_NONE,
     QL_EGRAPH_RULE_CONSTANT_BOOL_TRUE, QL_RULE_OP_NONE, QL_RULE_OP_NONE,
     {0u, 1u}, 0u, 0u},
    {"equal.symmetric", "equal(a, b) = equal(b, a)",
     QL_EGRAPH_RULE_SHAPE_COMMUTATIVE, 1u, {QL_EGRAPH_OP_EQUAL, 0u, 0u, 0u},
     2u, QL_RULE_ST, QL_RULE_NONE, QL_RULE_NONE, QL_RULE_OP_NONE,
     QL_RULE_OP_NONE, {0u, 0u}, 0u, 0u},
    {"ite.true", "ite(true, t, e) = t",
     QL_EGRAPH_RULE_SHAPE_SELECT_BRANCH, 1u, {QL_EGRAPH_OP_ITE, 0u, 0u, 0u},
     3u, QL_RULE_ST | QL_RULE_WC, QL_EGRAPH_RULE_CONSTANT_BOOL_TRUE,
     QL_RULE_NONE, 0u, 1u, {0u, 0u}, 0u, 0u},
    {"ite.false", "ite(false, t, e) = e",
     QL_EGRAPH_RULE_SHAPE_SELECT_BRANCH, 1u, {QL_EGRAPH_OP_ITE, 0u, 0u, 0u},
     3u, QL_RULE_ST | QL_RULE_WC, QL_EGRAPH_RULE_CONSTANT_BOOL_FALSE,
     QL_RULE_NONE, 0u, 2u, {0u, 0u}, 0u, 0u},
    {"ite.same",
     "ite(c, t, t) = t; both branches must already share a class and the "
     "condition is then irrelevant because the term engine has no effects",
     QL_EGRAPH_RULE_SHAPE_SELECT_SAME, 1u, {QL_EGRAPH_OP_ITE, 0u, 0u, 0u},
     3u, QL_RULE_ST | QL_RULE_EQ, QL_RULE_NONE, QL_RULE_NONE,
     QL_RULE_OP_NONE, 1u, {1u, 2u}, 0u, 0u}
};

#define QL_EGRAPH_RULE_COUNT \
    (sizeof(ql_egraph_rules) / sizeof(ql_egraph_rules[0]))

static void fill_rule_descriptor(const ql_egraph_rule_entry *entry,
                                 ql_egraph_rule_descriptor_v1 *descriptor) {
    size_t index;
    memset(descriptor, 0, sizeof(*descriptor));
    descriptor->struct_size = sizeof(*descriptor);
    descriptor->abi_version = QL_ABI_VERSION;
    descriptor->catalogue_version = QL_EGRAPH_RULE_CATALOGUE_VERSION;
    descriptor->rule_name = entry->name;
    descriptor->soundness = entry->soundness;
    descriptor->shape = entry->shape;
    descriptor->subject_op_count = entry->op_count;
    for (index = 0u; index < QL_EGRAPH_RULE_MAX_SUBJECT_OPS; ++index) {
        descriptor->subject_ops[index] = entry->ops[index];
    }
    descriptor->subject_arity = entry->arity;
    descriptor->conditions = entry->conditions;
    descriptor->witness_constant = entry->witness;
    descriptor->result_constant = entry->result;
    descriptor->witness_operand = entry->witness_operand;
    descriptor->result_operand = entry->result_operand;
    descriptor->equal_operands[0] = entry->equal_operands[0];
    descriptor->equal_operands[1] = entry->equal_operands[1];
    descriptor->minimum_bit_width = entry->minimum_bit_width;
    descriptor->maximum_bit_width = entry->maximum_bit_width;
}

uint32_t QL_CALL ql_egraph_rule_catalogue_size(void) {
    return (uint32_t)QL_EGRAPH_RULE_COUNT;
}

ql_status QL_CALL ql_egraph_rule_catalogue_at(
    uint32_t index, ql_egraph_rule_descriptor_v1 *descriptor,
    ql_error *error) {
    if (descriptor == NULL) {
        ql_error_set(error, QL_STATUS_INVALID_ARGUMENT,
                     "e-graph rule descriptor output is required");
        return QL_STATUS_INVALID_ARGUMENT;
    }
    if ((size_t)index >= QL_EGRAPH_RULE_COUNT) {
        ql_error_set(error, QL_STATUS_NOT_FOUND,
                     "e-graph rule index %u is out of range (%u rules)",
                     index, (unsigned)QL_EGRAPH_RULE_COUNT);
        return QL_STATUS_NOT_FOUND;
    }
    fill_rule_descriptor(&ql_egraph_rules[index], descriptor);
    ql_error_clear(error);
    return QL_STATUS_OK;
}

ql_status QL_CALL ql_egraph_rule_lookup(
    const char *rule_name, ql_egraph_rule_descriptor_v1 *descriptor,
    ql_error *error) {
    size_t index;
    if (rule_name == NULL || descriptor == NULL) {
        ql_error_set(error, QL_STATUS_INVALID_ARGUMENT,
                     "e-graph rule lookup needs a name and an output");
        return QL_STATUS_INVALID_ARGUMENT;
    }
    for (index = 0u; index < QL_EGRAPH_RULE_COUNT; ++index) {
        if (strcmp(ql_egraph_rules[index].name, rule_name) == 0) {
            fill_rule_descriptor(&ql_egraph_rules[index], descriptor);
            ql_error_clear(error);
            return QL_STATUS_OK;
        }
    }
    ql_error_set(error, QL_STATUS_NOT_FOUND,
                 "no e-graph rewrite rule is named '%s'", rule_name);
    return QL_STATUS_NOT_FOUND;
}

const char *QL_CALL ql_egraph_rule_shape_string(
    ql_egraph_rule_shape shape) {
    switch (shape) {
    case QL_EGRAPH_RULE_SHAPE_COMMUTATIVE:
        return "commutative";
    case QL_EGRAPH_RULE_SHAPE_IDEMPOTENT:
        return "idempotent";
    case QL_EGRAPH_RULE_SHAPE_INVOLUTION:
        return "involution";
    case QL_EGRAPH_RULE_SHAPE_IDENTITY_ELEMENT:
        return "identity-element";
    case QL_EGRAPH_RULE_SHAPE_ABSORBING_ELEMENT:
        return "absorbing-element";
    case QL_EGRAPH_RULE_SHAPE_SELF_ANNIHILATION:
        return "self-annihilation";
    case QL_EGRAPH_RULE_SHAPE_CONSTANT_FOLD:
        return "constant-fold";
    case QL_EGRAPH_RULE_SHAPE_SELECT_BRANCH:
        return "select-branch";
    case QL_EGRAPH_RULE_SHAPE_SELECT_SAME:
        return "select-same";
    case QL_EGRAPH_RULE_SHAPE_REFLEXIVE:
        return "reflexive";
    default:
        return "invalid";
    }
}

const char *QL_CALL ql_egraph_rule_constant_string(
    ql_egraph_rule_constant constant) {
    switch (constant) {
    case QL_EGRAPH_RULE_CONSTANT_BOOL_FALSE:
        return "bool.false";
    case QL_EGRAPH_RULE_CONSTANT_BOOL_TRUE:
        return "bool.true";
    case QL_EGRAPH_RULE_CONSTANT_BV_ZERO:
        return "bv.zero";
    case QL_EGRAPH_RULE_CONSTANT_BV_ONE:
        return "bv.one";
    case QL_EGRAPH_RULE_CONSTANT_BV_ONES:
        return "bv.ones";
    default:
        return "none";
    }
}

/* Canonical serialization: one line per rule in catalogue order, fields in
   declaration order, ASCII decimal, no locale dependence. It is streamed into
   the hasher rather than buffered so the function needs no allocator and no
   shared state. Any change to a premise moves the digest, which is what
   evidence and cache keys rely on. */
#define QL_EGRAPH_RULE_LINE_CAPACITY 512u

static ql_status hash_rule_chunk(blake3_hasher *hasher, ql_error *error,
                                 const char *format, ...) {
    char line[QL_EGRAPH_RULE_LINE_CAPACITY];
    va_list arguments;
    int written;

    va_start(arguments, format);
    written = vsnprintf(line, sizeof(line), format, arguments);
    va_end(arguments);
    if (written < 0 || (size_t)written >= sizeof(line)) {
        ql_error_set(error, QL_STATUS_INTERNAL_ERROR,
                     "e-graph rule catalogue serialization overflowed");
        return QL_STATUS_INTERNAL_ERROR;
    }
    blake3_hasher_update(hasher, line, (size_t)written);
    return QL_STATUS_OK;
}

ql_status QL_CALL ql_egraph_rule_catalogue_digest(ql_digest *digest,
                                                  ql_error *error) {
    blake3_hasher hasher;
    size_t index;

    if (digest == NULL) {
        ql_error_set(error, QL_STATUS_INVALID_ARGUMENT,
                     "e-graph rule catalogue digest output is required");
        return QL_STATUS_INVALID_ARGUMENT;
    }
    blake3_hasher_init(&hasher);
    if (hash_rule_chunk(&hasher, error,
                        "quodlibet.egraph.rules\nversion=%u\ncount=%u\n",
                        (unsigned)QL_EGRAPH_RULE_CATALOGUE_VERSION,
                        (unsigned)QL_EGRAPH_RULE_COUNT) != QL_STATUS_OK) {
        return QL_STATUS_INTERNAL_ERROR;
    }
    for (index = 0u; index < QL_EGRAPH_RULE_COUNT; ++index) {
        const ql_egraph_rule_entry *entry = &ql_egraph_rules[index];
        size_t op;
        if (hash_rule_chunk(&hasher, error,
                            "%s|%u|%u|%u|%u|%u|%u|%u|%u|%u|%u|%u",
                            entry->name, (unsigned)entry->shape,
                            (unsigned)entry->arity,
                            (unsigned)entry->conditions,
                            (unsigned)entry->witness,
                            (unsigned)entry->result,
                            (unsigned)entry->witness_operand,
                            (unsigned)entry->result_operand,
                            (unsigned)entry->equal_operands[0],
                            (unsigned)entry->equal_operands[1],
                            (unsigned)entry->minimum_bit_width,
                            (unsigned)entry->maximum_bit_width) !=
            QL_STATUS_OK) {
            return QL_STATUS_INTERNAL_ERROR;
        }
        for (op = 0u; op < (size_t)entry->op_count; ++op) {
            if (hash_rule_chunk(&hasher, error, "|op%u",
                                (unsigned)entry->ops[op]) != QL_STATUS_OK) {
                return QL_STATUS_INTERNAL_ERROR;
            }
        }
        blake3_hasher_update(&hasher, "|", 1u);
        blake3_hasher_update(&hasher, entry->soundness,
                             strlen(entry->soundness));
        blake3_hasher_update(&hasher, "\n", 1u);
    }
    blake3_hasher_finalize(&hasher, digest->bytes, QL_DIGEST_SIZE);
    ql_error_clear(error);
    return QL_STATUS_OK;
}
