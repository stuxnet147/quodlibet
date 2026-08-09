#include "quodlibet/product.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "quodlibet/precondition.h"

#define QL_PRODUCT_SYMBOL_CAPACITY 40u
#define QL_PRODUCT_MAX_BV_WIDTH 1024u

#define QL_PRODUCT_PRECONDITION_SYMBOL "quodlibet_precondition"
#define QL_PRODUCT_OBSERVATION_SYMBOL "quodlibet_observation_equal"
#define QL_PRODUCT_DOMAIN_SYMBOL "quodlibet_domain"
#define QL_PRODUCT_VIOLATION_SYMBOL "quodlibet_violation"

static const char product_violation_assertion[] =
    "(assert " QL_PRODUCT_VIOLATION_SYMBOL ")\n";
static const char product_domain_assertion[] =
    "(assert " QL_PRODUCT_DOMAIN_SYMBOL ")\n";

typedef struct product_buffer {
    ql_allocator allocator;
    char *data;
    size_t size;
    size_t capacity;
} product_buffer;

/* One terminal or guard site: the block whose reachability enables it, plus
   the value or immediate it carries. */
typedef struct product_site {
    ql_ir_block_id block;
    ql_ir_value_id value;
    uint64_t code;
} product_site;

typedef struct product_site_list {
    product_site *items;
    size_t count;
    size_t capacity;
} product_site_list;

typedef struct product_edge {
    ql_ir_block_id from;
    ql_ir_block_id to;
} product_edge;

typedef struct product_side {
    const ql_ir *ir;
    ql_ir_view_v1 view;
    char prefix;
    char *value_symbols;
    ql_ir_type_kind *value_kinds;
    uint32_t *value_widths;
    product_edge *edges;
    size_t edge_count;
    ql_ir_block_id *order;
    product_site_list guards;
    product_site_list returns;
    product_site_list traps;
    product_site_list undefined;
    product_site_list diverges;
    ql_ir_type_kind return_kind;
    uint32_t return_width;
} product_side;

typedef struct product_encoder {
    const ql_allocator *allocator;
    ql_smt2_builder *builder;
    product_buffer term;
    const ql_product_input_v1 *inputs;
    size_t input_count;
    ql_error *error;
} product_encoder;

struct ql_product_query {
    ql_allocator allocator;
    ql_product_query_view_v1 view;
    ql_product_input_v1 *inputs;
    char *input_symbols;
    ql_artifact *prefix;
    ql_artifact *violation;
    ql_artifact *domain;
};

/* --- Text buffer ---------------------------------------------------------- */

static void buffer_init(product_buffer *buffer, const ql_allocator *allocator) {
    memset(buffer, 0, sizeof(*buffer));
    buffer->allocator = *allocator;
}

static void buffer_dispose(product_buffer *buffer) {
    if (buffer->data != NULL) {
        buffer->allocator.deallocate(buffer->allocator.user_data,
                                     buffer->data);
    }
    memset(buffer, 0, sizeof(*buffer));
}

static ql_status buffer_append(product_buffer *buffer, const char *text,
                               ql_error *error) {
    const size_t length = strlen(text);
    size_t capacity;
    void *allocation;

    if (length + 1u > SIZE_MAX - buffer->size) {
        ql_error_set(error, QL_STATUS_OUT_OF_MEMORY, "product term overflow");
        return QL_STATUS_OUT_OF_MEMORY;
    }
    if (buffer->size + length + 1u > buffer->capacity) {
        capacity = buffer->capacity == 0u ? 256u : buffer->capacity;
        while (capacity < buffer->size + length + 1u) {
            if (capacity > SIZE_MAX / 2u) {
                capacity = buffer->size + length + 1u;
                break;
            }
            capacity *= 2u;
        }
        allocation = buffer->allocator.reallocate(
            buffer->allocator.user_data, buffer->data, capacity);
        if (allocation == NULL) {
            ql_error_set(error, QL_STATUS_OUT_OF_MEMORY,
                         "could not grow the product term buffer");
            return QL_STATUS_OUT_OF_MEMORY;
        }
        buffer->data = (char *)allocation;
        buffer->capacity = capacity;
    }
    memcpy(buffer->data + buffer->size, text, length);
    buffer->size += length;
    buffer->data[buffer->size] = '\0';
    return QL_STATUS_OK;
}

static void buffer_reset(product_buffer *buffer) {
    buffer->size = 0u;
    if (buffer->data != NULL) {
        buffer->data[0] = '\0';
    }
}

static const char *buffer_text(const product_buffer *buffer) {
    return buffer->data != NULL ? buffer->data : "";
}

static ql_status term_add(product_encoder *encoder, const char *text) {
    return buffer_append(&encoder->term, text, encoder->error);
}

/* Views expose sized rather than NUL-terminated text, so copy through a
   bounded buffer instead of assuming a terminator. */
static ql_status term_add_bytes(product_encoder *encoder, const char *bytes,
                                size_t size) {
    char scratch[80];

    if (size + 1u > sizeof(scratch)) {
        ql_error_set(encoder->error, QL_STATUS_TYPE_MISMATCH,
                     "a precondition literal is longer than the miter accepts");
        return QL_STATUS_TYPE_MISMATCH;
    }
    memcpy(scratch, bytes, size);
    scratch[size] = '\0';
    return buffer_append(&encoder->term, scratch, encoder->error);
}

static ql_status term_addf(product_encoder *encoder, const char *format, ...) {
    char scratch[QL_PRODUCT_SYMBOL_CAPACITY + 32u];
    va_list arguments;
    int count;

    va_start(arguments, format);
    count = vsnprintf(scratch, sizeof(scratch), format, arguments);
    va_end(arguments);
    if (count < 0 || (size_t)count >= sizeof(scratch)) {
        ql_error_set(encoder->error, QL_STATUS_INTERNAL_ERROR,
                     "could not format a product symbol");
        return QL_STATUS_INTERNAL_ERROR;
    }
    return buffer_append(&encoder->term, scratch, encoder->error);
}

/* --- Site lists ----------------------------------------------------------- */

static ql_status site_list_add(product_site_list *list,
                               const ql_allocator *allocator,
                               ql_ir_block_id block, ql_ir_value_id value,
                               uint64_t code, ql_error *error) {
    if (list->count == list->capacity) {
        const size_t capacity = list->capacity == 0u ? 8u
                                                     : list->capacity * 2u;
        void *allocation = allocator->reallocate(
            allocator->user_data, list->items, capacity * sizeof(*list->items));
        if (allocation == NULL) {
            ql_error_set(error, QL_STATUS_OUT_OF_MEMORY, NULL);
            return QL_STATUS_OUT_OF_MEMORY;
        }
        list->items = (product_site *)allocation;
        list->capacity = capacity;
    }
    list->items[list->count].block = block;
    list->items[list->count].value = value;
    list->items[list->count].code = code;
    ++list->count;
    return QL_STATUS_OK;
}

static void site_list_dispose(product_site_list *list,
                              const ql_allocator *allocator) {
    allocator->deallocate(allocator->user_data, list->items);
    memset(list, 0, sizeof(*list));
}

/* --- Fragment gate -------------------------------------------------------- */

static int opcode_is_supported(ql_ir_opcode opcode) {
    switch (opcode) {
    case QL_IR_OPCODE_IDENTITY:
    case QL_IR_OPCODE_PHI:
    case QL_IR_OPCODE_BOOL_NOT:
    case QL_IR_OPCODE_BV_NOT:
    case QL_IR_OPCODE_BV_NEG:
    case QL_IR_OPCODE_ADD:
    case QL_IR_OPCODE_SUB:
    case QL_IR_OPCODE_MUL:
    case QL_IR_OPCODE_UDIV:
    case QL_IR_OPCODE_SDIV:
    case QL_IR_OPCODE_UREM:
    case QL_IR_OPCODE_SREM:
    case QL_IR_OPCODE_SHL:
    case QL_IR_OPCODE_LSHR:
    case QL_IR_OPCODE_ASHR:
    case QL_IR_OPCODE_BV_AND:
    case QL_IR_OPCODE_BV_OR:
    case QL_IR_OPCODE_BV_XOR:
    case QL_IR_OPCODE_EQ:
    case QL_IR_OPCODE_NE:
    case QL_IR_OPCODE_ULT:
    case QL_IR_OPCODE_ULE:
    case QL_IR_OPCODE_SLT:
    case QL_IR_OPCODE_SLE:
    case QL_IR_OPCODE_SELECT:
    case QL_IR_OPCODE_ZEXT:
    case QL_IR_OPCODE_SEXT:
    case QL_IR_OPCODE_TRUNC:
    case QL_IR_OPCODE_UB_GUARD:
        return 1;
    default:
        return 0;
    }
}

/* Refuses everything the scalar miter cannot state, so no observation axis is
   ever silently dropped. */
static ql_status check_ir_fragment(const ql_ir *ir,
                                   const ql_ir_view_v1 *view,
                                   const char *side, ql_error *error) {
    size_t index;
    ql_status status;

    for (index = 0u; index < view->type_count; ++index) {
        ql_ir_type_view_v1 type;
        memset(&type, 0, sizeof(type));
        type.struct_size = sizeof(type);
        status = ql_ir_type_at(ir, index, &type, error);
        if (status != QL_STATUS_OK) {
            return status;
        }
        if (type.kind != QL_IR_TYPE_VOID && type.kind != QL_IR_TYPE_BOOL &&
            type.kind != QL_IR_TYPE_BIT_VECTOR) {
            ql_error_set(error, QL_STATUS_TYPE_MISMATCH,
                         "%s IR uses a non-scalar type; pointers, floats, aggregates, memory, and event traces are outside the loop-free scalar miter",
                         side);
            return QL_STATUS_TYPE_MISMATCH;
        }
        if (type.kind == QL_IR_TYPE_BIT_VECTOR &&
            type.bit_width > QL_PRODUCT_MAX_BV_WIDTH) {
            ql_error_set(error, QL_STATUS_TYPE_MISMATCH,
                         "%s IR uses a %u-bit vector above the %u-bit miter limit",
                         side, type.bit_width, QL_PRODUCT_MAX_BV_WIDTH);
            return QL_STATUS_TYPE_MISMATCH;
        }
    }
    for (index = 0u; index < view->instruction_count; ++index) {
        ql_ir_instruction_view_v1 instruction;
        memset(&instruction, 0, sizeof(instruction));
        instruction.struct_size = sizeof(instruction);
        status = ql_ir_instruction_at(ir, index, &instruction, error);
        if (status != QL_STATUS_OK) {
            return status;
        }
        if (!opcode_is_supported(instruction.opcode)) {
            ql_error_set(error, QL_STATUS_TYPE_MISMATCH,
                         "%s IR opcode %u is outside the loop-free scalar miter",
                         side, instruction.opcode);
            return QL_STATUS_TYPE_MISMATCH;
        }
        if (instruction.opcode == QL_IR_OPCODE_UB_GUARD) {
            continue;
        }
        if (instruction.effects != QL_IR_EFFECT_NONE) {
            ql_error_set(error, QL_STATUS_TYPE_MISMATCH,
                         "%s IR has a memory, call, volatile, atomic, or I/O effect that the scalar miter does not model",
                         side);
            return QL_STATUS_TYPE_MISMATCH;
        }
    }
    for (index = 0u; index < view->block_count; ++index) {
        ql_ir_block_view_v1 block;
        memset(&block, 0, sizeof(block));
        block.struct_size = sizeof(block);
        status = ql_ir_block_at(ir, index, &block, error);
        if (status != QL_STATUS_OK) {
            return status;
        }
        if (block.terminator.memory != QL_IR_INVALID_VALUE_ID ||
            block.terminator.event_trace != QL_IR_INVALID_VALUE_ID) {
            ql_error_set(error, QL_STATUS_TYPE_MISMATCH,
                         "%s IR observes terminal memory or an event trace, which the scalar miter does not model",
                         side);
            return QL_STATUS_TYPE_MISMATCH;
        }
        switch (block.terminator.kind) {
        case QL_IR_TERMINATOR_RETURN:
        case QL_IR_TERMINATOR_BRANCH:
        case QL_IR_TERMINATOR_COND_BRANCH:
        case QL_IR_TERMINATOR_TRAP:
        case QL_IR_TERMINATOR_UNDEFINED_BEHAVIOR:
        case QL_IR_TERMINATOR_DIVERGE:
            break;
        default:
            ql_error_set(error, QL_STATUS_TYPE_MISMATCH,
                         "%s IR terminator %u has no observation axis in this contract",
                         side, (unsigned)block.terminator.kind);
            return QL_STATUS_TYPE_MISMATCH;
        }
    }
    return QL_STATUS_OK;
}

/* --- Side setup ----------------------------------------------------------- */

static char *side_value_symbol(product_side *side, ql_ir_value_id value) {
    return side->value_symbols + (size_t)value * QL_PRODUCT_SYMBOL_CAPACITY;
}

static void side_dispose(product_side *side, const ql_allocator *allocator) {
    allocator->deallocate(allocator->user_data, side->value_symbols);
    allocator->deallocate(allocator->user_data, side->value_kinds);
    allocator->deallocate(allocator->user_data, side->value_widths);
    allocator->deallocate(allocator->user_data, side->edges);
    allocator->deallocate(allocator->user_data, side->order);
    site_list_dispose(&side->guards, allocator);
    site_list_dispose(&side->returns, allocator);
    site_list_dispose(&side->traps, allocator);
    site_list_dispose(&side->undefined, allocator);
    site_list_dispose(&side->diverges, allocator);
    memset(side, 0, sizeof(*side));
}

static ql_status side_type_of(const ql_ir *ir, const ql_ir_view_v1 *view,
                              ql_ir_type_id type_id, ql_ir_type_kind *kind,
                              uint32_t *width, ql_error *error) {
    ql_ir_type_view_v1 type;

    if (type_id >= view->type_count) {
        ql_error_set(error, QL_STATUS_TYPE_MISMATCH,
                     "IR references type %u outside the type table", type_id);
        return QL_STATUS_TYPE_MISMATCH;
    }
    memset(&type, 0, sizeof(type));
    type.struct_size = sizeof(type);
    if (ql_ir_type_at(ir, type_id, &type, error) != QL_STATUS_OK) {
        return QL_STATUS_TYPE_MISMATCH;
    }
    *kind = type.kind;
    *width = type.bit_width;
    return QL_STATUS_OK;
}

/* Kahn's algorithm over the branch graph. ql_ir_open() has already proved the
   graph acyclic and fully reachable, so a shortfall here is an internal
   inconsistency rather than a user error. */
static ql_status side_order_blocks(product_side *side,
                                   const ql_allocator *allocator,
                                   ql_error *error) {
    size_t *in_degree;
    size_t placed = 0u;
    size_t cursor = 0u;
    size_t index;

    in_degree = allocator->allocate(allocator->user_data,
                                    side->view.block_count *
                                        sizeof(*in_degree));
    if (in_degree == NULL) {
        ql_error_set(error, QL_STATUS_OUT_OF_MEMORY, NULL);
        return QL_STATUS_OUT_OF_MEMORY;
    }
    memset(in_degree, 0, side->view.block_count * sizeof(*in_degree));
    for (index = 0u; index < side->edge_count; ++index) {
        ++in_degree[side->edges[index].to];
    }
    for (index = 0u; index < side->view.block_count; ++index) {
        if (in_degree[index] == 0u) {
            side->order[placed++] = (ql_ir_block_id)index;
        }
    }
    while (cursor < placed) {
        const ql_ir_block_id block = side->order[cursor++];
        for (index = 0u; index < side->edge_count; ++index) {
            if (side->edges[index].from != block) {
                continue;
            }
            if (--in_degree[side->edges[index].to] == 0u) {
                side->order[placed++] = side->edges[index].to;
            }
        }
    }
    allocator->deallocate(allocator->user_data, in_degree);
    if (placed != side->view.block_count) {
        ql_error_set(error, QL_STATUS_INTERNAL_ERROR,
                     "IR control-flow graph is not acyclic");
        return QL_STATUS_INTERNAL_ERROR;
    }
    return QL_STATUS_OK;
}

static ql_status side_collect_edges(product_side *side,
                                    const ql_allocator *allocator,
                                    ql_error *error) {
    size_t capacity = side->view.block_count * 2u + 1u;
    size_t index;
    ql_status status;

    side->edges = allocator->allocate(allocator->user_data,
                                      capacity * sizeof(*side->edges));
    if (side->edges == NULL) {
        ql_error_set(error, QL_STATUS_OUT_OF_MEMORY, NULL);
        return QL_STATUS_OUT_OF_MEMORY;
    }
    for (index = 0u; index < side->view.block_count; ++index) {
        ql_ir_block_view_v1 block;
        memset(&block, 0, sizeof(block));
        block.struct_size = sizeof(block);
        status = ql_ir_block_at(side->ir, index, &block, error);
        if (status != QL_STATUS_OK) {
            return status;
        }
        if (block.terminator.kind == QL_IR_TERMINATOR_BRANCH) {
            side->edges[side->edge_count].from = block.id;
            side->edges[side->edge_count].to = block.terminator.target;
            ++side->edge_count;
        } else if (block.terminator.kind == QL_IR_TERMINATOR_COND_BRANCH) {
            side->edges[side->edge_count].from = block.id;
            side->edges[side->edge_count].to = block.terminator.target;
            ++side->edge_count;
            if (block.terminator.false_target != block.terminator.target) {
                side->edges[side->edge_count].from = block.id;
                side->edges[side->edge_count].to =
                    block.terminator.false_target;
                ++side->edge_count;
            }
        }
    }
    return QL_STATUS_OK;
}

static ql_status side_prepare(product_side *side, const ql_ir *ir,
                              char prefix, const ql_allocator *allocator,
                              const ql_product_input_v1 *inputs,
                              size_t input_count, ql_error *error) {
    size_t index;
    size_t parameter_index = 0u;
    ql_status status;

    memset(side, 0, sizeof(*side));
    side->ir = ir;
    side->prefix = prefix;
    side->view.struct_size = sizeof(side->view);
    status = ql_ir_get_view(ir, &side->view, error);
    if (status != QL_STATUS_OK) {
        return status;
    }
    side->value_symbols = allocator->allocate(
        allocator->user_data,
        side->view.value_count * QL_PRODUCT_SYMBOL_CAPACITY);
    side->value_kinds = allocator->allocate(
        allocator->user_data,
        side->view.value_count * sizeof(*side->value_kinds));
    side->value_widths = allocator->allocate(
        allocator->user_data,
        side->view.value_count * sizeof(*side->value_widths));
    side->order = allocator->allocate(
        allocator->user_data, side->view.block_count * sizeof(*side->order));
    if (side->value_symbols == NULL || side->value_kinds == NULL ||
        side->value_widths == NULL || side->order == NULL) {
        ql_error_set(error, QL_STATUS_OUT_OF_MEMORY, NULL);
        return QL_STATUS_OUT_OF_MEMORY;
    }

    for (index = 0u; index < side->view.value_count; ++index) {
        ql_ir_value_view_v1 value;
        char *symbol = side_value_symbol(side, (ql_ir_value_id)index);
        int written;

        memset(&value, 0, sizeof(value));
        value.struct_size = sizeof(value);
        status = ql_ir_value_at(ir, index, &value, error);
        if (status != QL_STATUS_OK) {
            return status;
        }
        status = side_type_of(ir, &side->view, value.type,
                              &side->value_kinds[index],
                              &side->value_widths[index], error);
        if (status != QL_STATUS_OK) {
            return status;
        }
        if (value.definition_kind == QL_IR_VALUE_PARAMETER) {
            /* Both sides share one symbol per corresponding argument. */
            size_t input;
            for (input = 0u; input < input_count; ++input) {
                const uint32_t ordinal = prefix == 'l'
                                             ? inputs[input].left_parameter
                                             : inputs[input].right_parameter;
                if ((size_t)ordinal == parameter_index) {
                    break;
                }
            }
            if (input == input_count) {
                ql_error_set(error, QL_STATUS_TYPE_MISMATCH,
                             "IR parameter %zu has no argument correspondence",
                             parameter_index);
                return QL_STATUS_TYPE_MISMATCH;
            }
            written = snprintf(symbol, QL_PRODUCT_SYMBOL_CAPACITY, "%s",
                               inputs[input].symbol);
            ++parameter_index;
        } else {
            written = snprintf(symbol, QL_PRODUCT_SYMBOL_CAPACITY, "%c_v%u",
                               prefix, (unsigned)index);
        }
        if (written < 0 || (size_t)written >= QL_PRODUCT_SYMBOL_CAPACITY) {
            ql_error_set(error, QL_STATUS_INTERNAL_ERROR,
                         "could not format an IR value symbol");
            return QL_STATUS_INTERNAL_ERROR;
        }
    }
    status = side_type_of(ir, &side->view, side->view.return_type,
                          &side->return_kind, &side->return_width, error);
    if (status != QL_STATUS_OK) {
        return status;
    }
    status = side_collect_edges(side, allocator, error);
    if (status != QL_STATUS_OK) {
        return status;
    }
    return side_order_blocks(side, allocator, error);
}

/* --- Term emission -------------------------------------------------------- */

static ql_status emit_bool(product_encoder *encoder, const char *symbol) {
    return ql_smt2_builder_define_bool(encoder->builder, symbol,
                                       buffer_text(&encoder->term),
                                       encoder->error);
}

static ql_status emit_bv(product_encoder *encoder, const char *symbol,
                         uint32_t width) {
    return ql_smt2_builder_define_bv(encoder->builder, symbol, width,
                                     buffer_text(&encoder->term),
                                     encoder->error);
}

static ql_status emit_sorted(product_encoder *encoder, const char *symbol,
                             ql_ir_type_kind kind, uint32_t width) {
    if (kind == QL_IR_TYPE_BOOL) {
        return emit_bool(encoder, symbol);
    }
    return emit_bv(encoder, symbol, width);
}

static ql_status term_constant(product_encoder *encoder,
                               ql_ir_type_kind kind, uint32_t width,
                               const void *data, size_t size) {
    const unsigned char *bytes = (const unsigned char *)data;
    char scratch[2];
    uint32_t bit;

    if (kind == QL_IR_TYPE_BOOL) {
        if (size != 1u) {
            ql_error_set(encoder->error, QL_STATUS_TYPE_MISMATCH,
                         "IR boolean constant is not one byte");
            return QL_STATUS_TYPE_MISMATCH;
        }
        return term_add(encoder, bytes[0] != 0u ? "true" : "false");
    }
    if (width == 0u || size != ((size_t)width + 7u) / 8u) {
        ql_error_set(encoder->error, QL_STATUS_TYPE_MISMATCH,
                     "IR bit-vector constant has an unexpected byte count");
        return QL_STATUS_TYPE_MISMATCH;
    }
    if (term_add(encoder, "#b") != QL_STATUS_OK) {
        return QL_STATUS_OUT_OF_MEMORY;
    }
    scratch[1] = '\0';
    for (bit = width; bit != 0u; --bit) {
        const uint32_t position = bit - 1u;
        const unsigned char byte = bytes[position / 8u];
        scratch[0] = ((byte >> (position % 8u)) & 1u) != 0u ? '1' : '0';
        if (term_add(encoder, scratch) != QL_STATUS_OK) {
            return QL_STATUS_OUT_OF_MEMORY;
        }
    }
    return QL_STATUS_OK;
}

static ql_status term_zero(product_encoder *encoder, ql_ir_type_kind kind,
                           uint32_t width) {
    uint32_t bit;

    if (kind == QL_IR_TYPE_BOOL) {
        return term_add(encoder, "false");
    }
    if (term_add(encoder, "#b") != QL_STATUS_OK) {
        return QL_STATUS_OUT_OF_MEMORY;
    }
    for (bit = 0u; bit < width; ++bit) {
        if (term_add(encoder, "0") != QL_STATUS_OK) {
            return QL_STATUS_OUT_OF_MEMORY;
        }
    }
    return QL_STATUS_OK;
}

static const char *binary_operator_name(ql_ir_opcode opcode) {
    switch (opcode) {
    case QL_IR_OPCODE_ADD:
        return "bvadd";
    case QL_IR_OPCODE_SUB:
        return "bvsub";
    case QL_IR_OPCODE_MUL:
        return "bvmul";
    case QL_IR_OPCODE_UDIV:
        return "bvudiv";
    case QL_IR_OPCODE_SDIV:
        return "bvsdiv";
    case QL_IR_OPCODE_UREM:
        return "bvurem";
    case QL_IR_OPCODE_SREM:
        return "bvsrem";
    case QL_IR_OPCODE_SHL:
        return "bvshl";
    case QL_IR_OPCODE_LSHR:
        return "bvlshr";
    case QL_IR_OPCODE_ASHR:
        return "bvashr";
    case QL_IR_OPCODE_BV_AND:
        return "bvand";
    case QL_IR_OPCODE_BV_OR:
        return "bvor";
    case QL_IR_OPCODE_BV_XOR:
        return "bvxor";
    case QL_IR_OPCODE_ULT:
        return "bvult";
    case QL_IR_OPCODE_ULE:
        return "bvule";
    case QL_IR_OPCODE_SLT:
        return "bvslt";
    case QL_IR_OPCODE_SLE:
        return "bvsle";
    default:
        return NULL;
    }
}

static ql_status term_edge_symbol(product_encoder *encoder,
                                  const product_side *side,
                                  ql_ir_block_id from, ql_ir_block_id to) {
    return term_addf(encoder, "%c_e%u_%u", side->prefix, (unsigned)from,
                     (unsigned)to);
}

static ql_status term_block_symbol(product_encoder *encoder,
                                   const product_side *side,
                                   ql_ir_block_id block) {
    return term_addf(encoder, "%c_b%u", side->prefix, (unsigned)block);
}

static ql_status encode_instruction(product_encoder *encoder,
                                    product_side *side,
                                    const ql_ir_instruction_view_v1 *view,
                                    ql_ir_block_id block,
                                    const ql_allocator *allocator) {
    const char *name;
    ql_ir_value_id result;
    ql_ir_type_kind kind;
    uint32_t width;
    size_t index;
    ql_status status;

    if (view->opcode == QL_IR_OPCODE_UB_GUARD) {
        return site_list_add(&side->guards, allocator, block,
                             view->operands[0], 0u, encoder->error);
    }
    result = view->results[0];
    kind = side->value_kinds[result];
    width = side->value_widths[result];
    buffer_reset(&encoder->term);

    switch (view->opcode) {
    case QL_IR_OPCODE_IDENTITY:
        status = term_add(encoder,
                          side_value_symbol(side, view->operands[0]));
        break;
    case QL_IR_OPCODE_BOOL_NOT:
        status = term_add(encoder, "(not ");
        if (status == QL_STATUS_OK) {
            status = term_add(encoder,
                              side_value_symbol(side, view->operands[0]));
        }
        if (status == QL_STATUS_OK) {
            status = term_add(encoder, ")");
        }
        break;
    case QL_IR_OPCODE_BV_NOT:
    case QL_IR_OPCODE_BV_NEG:
        status = term_add(encoder, view->opcode == QL_IR_OPCODE_BV_NOT
                                       ? "(bvnot "
                                       : "(bvneg ");
        if (status == QL_STATUS_OK) {
            status = term_add(encoder,
                              side_value_symbol(side, view->operands[0]));
        }
        if (status == QL_STATUS_OK) {
            status = term_add(encoder, ")");
        }
        break;
    case QL_IR_OPCODE_EQ:
    case QL_IR_OPCODE_NE:
        status = term_add(encoder, view->opcode == QL_IR_OPCODE_NE
                                       ? "(not (= "
                                       : "(= ");
        if (status == QL_STATUS_OK) {
            status = term_add(encoder,
                              side_value_symbol(side, view->operands[0]));
        }
        if (status == QL_STATUS_OK) {
            status = term_add(encoder, " ");
        }
        if (status == QL_STATUS_OK) {
            status = term_add(encoder,
                              side_value_symbol(side, view->operands[1]));
        }
        if (status == QL_STATUS_OK) {
            status = term_add(encoder,
                              view->opcode == QL_IR_OPCODE_NE ? "))" : ")");
        }
        break;
    case QL_IR_OPCODE_SELECT:
        status = term_add(encoder, "(ite ");
        for (index = 0u; index < 3u && status == QL_STATUS_OK; ++index) {
            status = term_add(encoder,
                              side_value_symbol(side, view->operands[index]));
            if (status == QL_STATUS_OK) {
                status = term_add(encoder, index == 2u ? ")" : " ");
            }
        }
        break;
    case QL_IR_OPCODE_ZEXT:
    case QL_IR_OPCODE_SEXT:
        status = term_addf(
            encoder, "((_ %s %u) ",
            view->opcode == QL_IR_OPCODE_ZEXT ? "zero_extend" : "sign_extend",
            width - side->value_widths[view->operands[0]]);
        if (status == QL_STATUS_OK) {
            status = term_add(encoder,
                              side_value_symbol(side, view->operands[0]));
        }
        if (status == QL_STATUS_OK) {
            status = term_add(encoder, ")");
        }
        break;
    case QL_IR_OPCODE_TRUNC:
        status = term_addf(encoder, "((_ extract %u 0) ", width - 1u);
        if (status == QL_STATUS_OK) {
            status = term_add(encoder,
                              side_value_symbol(side, view->operands[0]));
        }
        if (status == QL_STATUS_OK) {
            status = term_add(encoder, ")");
        }
        break;
    case QL_IR_OPCODE_PHI:
        status = QL_STATUS_OK;
        for (index = 0u; index + 1u < view->operand_count &&
                         status == QL_STATUS_OK;
             ++index) {
            status = term_add(encoder, "(ite ");
            if (status == QL_STATUS_OK) {
                status = term_edge_symbol(encoder, side,
                                          view->block_operands[index], block);
            }
            if (status == QL_STATUS_OK) {
                status = term_add(encoder, " ");
            }
            if (status == QL_STATUS_OK) {
                status = term_add(
                    encoder, side_value_symbol(side, view->operands[index]));
            }
            if (status == QL_STATUS_OK) {
                status = term_add(encoder, " ");
            }
        }
        if (status == QL_STATUS_OK) {
            status = term_add(
                encoder,
                side_value_symbol(side,
                                  view->operands[view->operand_count - 1u]));
        }
        for (index = 0u; index + 1u < view->operand_count &&
                         status == QL_STATUS_OK;
             ++index) {
            status = term_add(encoder, ")");
        }
        break;
    default:
        name = binary_operator_name(view->opcode);
        if (name == NULL) {
            ql_error_set(encoder->error, QL_STATUS_TYPE_MISMATCH,
                         "IR opcode %u is outside the loop-free scalar miter",
                         view->opcode);
            return QL_STATUS_TYPE_MISMATCH;
        }
        status = term_add(encoder, "(");
        if (status == QL_STATUS_OK) {
            status = term_add(encoder, name);
        }
        if (status == QL_STATUS_OK) {
            status = term_add(encoder, " ");
        }
        if (status == QL_STATUS_OK) {
            status = term_add(encoder,
                              side_value_symbol(side, view->operands[0]));
        }
        if (status == QL_STATUS_OK) {
            status = term_add(encoder, " ");
        }
        if (status == QL_STATUS_OK) {
            status = term_add(encoder,
                              side_value_symbol(side, view->operands[1]));
        }
        if (status == QL_STATUS_OK) {
            status = term_add(encoder, ")");
        }
        break;
    }
    if (status != QL_STATUS_OK) {
        return status;
    }
    return emit_sorted(encoder, side_value_symbol(side, result), kind, width);
}

static ql_status encode_constants(product_encoder *encoder,
                                  product_side *side) {
    size_t index;
    ql_status status;

    for (index = 0u; index < side->view.value_count; ++index) {
        ql_ir_value_view_v1 value;
        memset(&value, 0, sizeof(value));
        value.struct_size = sizeof(value);
        status = ql_ir_value_at(side->ir, index, &value, encoder->error);
        if (status != QL_STATUS_OK) {
            return status;
        }
        if (value.definition_kind != QL_IR_VALUE_CONSTANT) {
            continue;
        }
        buffer_reset(&encoder->term);
        status = term_constant(encoder, side->value_kinds[index],
                               side->value_widths[index], value.constant_data,
                               value.constant_size);
        if (status != QL_STATUS_OK) {
            return status;
        }
        status = emit_sorted(encoder,
                             side_value_symbol(side, (ql_ir_value_id)index),
                             side->value_kinds[index],
                             side->value_widths[index]);
        if (status != QL_STATUS_OK) {
            return status;
        }
    }
    return QL_STATUS_OK;
}

static ql_status encode_block_reach(product_encoder *encoder,
                                    product_side *side,
                                    ql_ir_block_id block) {
    char symbol[QL_PRODUCT_SYMBOL_CAPACITY];
    size_t incoming = 0u;
    size_t index;
    ql_status status;

    (void)snprintf(symbol, sizeof(symbol), "%c_b%u", side->prefix,
                   (unsigned)block);
    buffer_reset(&encoder->term);
    for (index = 0u; index < side->edge_count; ++index) {
        if (side->edges[index].to == block) {
            ++incoming;
        }
    }
    if (incoming == 0u) {
        status = term_add(encoder, "true");
    } else if (incoming == 1u) {
        status = QL_STATUS_OK;
        for (index = 0u; index < side->edge_count; ++index) {
            if (side->edges[index].to == block) {
                status = term_edge_symbol(encoder, side,
                                          side->edges[index].from, block);
                break;
            }
        }
    } else {
        status = term_add(encoder, "(or");
        for (index = 0u; index < side->edge_count && status == QL_STATUS_OK;
             ++index) {
            if (side->edges[index].to != block) {
                continue;
            }
            status = term_add(encoder, " ");
            if (status == QL_STATUS_OK) {
                status = term_edge_symbol(encoder, side,
                                          side->edges[index].from, block);
            }
        }
        if (status == QL_STATUS_OK) {
            status = term_add(encoder, ")");
        }
    }
    if (status != QL_STATUS_OK) {
        return status;
    }
    return emit_bool(encoder, symbol);
}

static ql_status encode_block_edges(product_encoder *encoder,
                                    product_side *side,
                                    const ql_ir_block_view_v1 *block) {
    char symbol[QL_PRODUCT_SYMBOL_CAPACITY];
    ql_status status;

    if (block->terminator.kind == QL_IR_TERMINATOR_BRANCH) {
        (void)snprintf(symbol, sizeof(symbol), "%c_e%u_%u", side->prefix,
                       (unsigned)block->id,
                       (unsigned)block->terminator.target);
        buffer_reset(&encoder->term);
        status = term_block_symbol(encoder, side, block->id);
        if (status != QL_STATUS_OK) {
            return status;
        }
        return emit_bool(encoder, symbol);
    }
    if (block->terminator.kind != QL_IR_TERMINATOR_COND_BRANCH) {
        return QL_STATUS_OK;
    }
    if (block->terminator.target == block->terminator.false_target) {
        (void)snprintf(symbol, sizeof(symbol), "%c_e%u_%u", side->prefix,
                       (unsigned)block->id,
                       (unsigned)block->terminator.target);
        buffer_reset(&encoder->term);
        status = term_block_symbol(encoder, side, block->id);
        if (status != QL_STATUS_OK) {
            return status;
        }
        return emit_bool(encoder, symbol);
    }
    (void)snprintf(symbol, sizeof(symbol), "%c_e%u_%u", side->prefix,
                   (unsigned)block->id, (unsigned)block->terminator.target);
    buffer_reset(&encoder->term);
    status = term_add(encoder, "(and ");
    if (status == QL_STATUS_OK) {
        status = term_block_symbol(encoder, side, block->id);
    }
    if (status == QL_STATUS_OK) {
        status = term_add(encoder, " ");
    }
    if (status == QL_STATUS_OK) {
        status = term_add(
            encoder, side_value_symbol(side, block->terminator.condition));
    }
    if (status == QL_STATUS_OK) {
        status = term_add(encoder, ")");
    }
    if (status == QL_STATUS_OK) {
        status = emit_bool(encoder, symbol);
    }
    if (status != QL_STATUS_OK) {
        return status;
    }
    (void)snprintf(symbol, sizeof(symbol), "%c_e%u_%u", side->prefix,
                   (unsigned)block->id,
                   (unsigned)block->terminator.false_target);
    buffer_reset(&encoder->term);
    status = term_add(encoder, "(and ");
    if (status == QL_STATUS_OK) {
        status = term_block_symbol(encoder, side, block->id);
    }
    if (status == QL_STATUS_OK) {
        status = term_add(encoder, " (not ");
    }
    if (status == QL_STATUS_OK) {
        status = term_add(
            encoder, side_value_symbol(side, block->terminator.condition));
    }
    if (status == QL_STATUS_OK) {
        status = term_add(encoder, "))");
    }
    if (status == QL_STATUS_OK) {
        status = emit_bool(encoder, symbol);
    }
    return status;
}

static ql_status encode_reachability_disjunction(
    product_encoder *encoder, product_side *side,
    const product_site_list *list, const char *symbol) {
    size_t index;
    ql_status status;

    buffer_reset(&encoder->term);
    if (list->count == 0u) {
        status = term_add(encoder, "false");
    } else if (list->count == 1u) {
        status = term_block_symbol(encoder, side, list->items[0].block);
    } else {
        status = term_add(encoder, "(or");
        for (index = 0u; index < list->count && status == QL_STATUS_OK;
             ++index) {
            status = term_add(encoder, " ");
            if (status == QL_STATUS_OK) {
                status = term_block_symbol(encoder, side,
                                           list->items[index].block);
            }
        }
        if (status == QL_STATUS_OK) {
            status = term_add(encoder, ")");
        }
    }
    if (status != QL_STATUS_OK) {
        return status;
    }
    return emit_bool(encoder, symbol);
}

static ql_status encode_side_aggregates(product_encoder *encoder,
                                        product_side *side) {
    char symbol[QL_PRODUCT_SYMBOL_CAPACITY];
    size_t index;
    ql_status status;

    /* defined: every reached UB guard holds and no UB terminator is reached */
    (void)snprintf(symbol, sizeof(symbol), "%c_defined", side->prefix);
    buffer_reset(&encoder->term);
    if (side->guards.count == 0u && side->undefined.count == 0u) {
        status = term_add(encoder, "true");
    } else {
        status = term_add(encoder, "(and");
        for (index = 0u; index < side->guards.count && status == QL_STATUS_OK;
             ++index) {
            status = term_add(encoder, " (=> ");
            if (status == QL_STATUS_OK) {
                status = term_block_symbol(encoder, side,
                                           side->guards.items[index].block);
            }
            if (status == QL_STATUS_OK) {
                status = term_add(encoder, " ");
            }
            if (status == QL_STATUS_OK) {
                status = term_add(
                    encoder,
                    side_value_symbol(side, side->guards.items[index].value));
            }
            if (status == QL_STATUS_OK) {
                status = term_add(encoder, ")");
            }
        }
        for (index = 0u;
             index < side->undefined.count && status == QL_STATUS_OK;
             ++index) {
            status = term_add(encoder, " (not ");
            if (status == QL_STATUS_OK) {
                status = term_block_symbol(
                    encoder, side, side->undefined.items[index].block);
            }
            if (status == QL_STATUS_OK) {
                status = term_add(encoder, ")");
            }
        }
        if (status == QL_STATUS_OK) {
            status = term_add(encoder, " true)");
        }
    }
    if (status == QL_STATUS_OK) {
        status = emit_bool(encoder, symbol);
    }
    if (status != QL_STATUS_OK) {
        return status;
    }

    (void)snprintf(symbol, sizeof(symbol), "%c_returns", side->prefix);
    status = encode_reachability_disjunction(encoder, side, &side->returns,
                                             symbol);
    if (status != QL_STATUS_OK) {
        return status;
    }
    (void)snprintf(symbol, sizeof(symbol), "%c_traps", side->prefix);
    status = encode_reachability_disjunction(encoder, side, &side->traps,
                                             symbol);
    if (status != QL_STATUS_OK) {
        return status;
    }
    (void)snprintf(symbol, sizeof(symbol), "%c_diverges", side->prefix);
    status = encode_reachability_disjunction(encoder, side, &side->diverges,
                                             symbol);
    if (status != QL_STATUS_OK) {
        return status;
    }

    /* The graph is acyclic and deterministic, so exactly one terminator is
       reached; termination is the complement of divergence. */
    (void)snprintf(symbol, sizeof(symbol), "%c_terminates", side->prefix);
    buffer_reset(&encoder->term);
    status = term_addf(encoder, "(not %c_diverges)", side->prefix);
    if (status == QL_STATUS_OK) {
        status = emit_bool(encoder, symbol);
    }
    if (status != QL_STATUS_OK) {
        return status;
    }

    if (side->return_kind != QL_IR_TYPE_VOID) {
        (void)snprintf(symbol, sizeof(symbol), "%c_return_value",
                       side->prefix);
        buffer_reset(&encoder->term);
        if (side->returns.count == 0u) {
            status = term_zero(encoder, side->return_kind,
                               side->return_width);
        } else {
            status = QL_STATUS_OK;
            for (index = 0u; index + 1u < side->returns.count &&
                             status == QL_STATUS_OK;
                 ++index) {
                status = term_add(encoder, "(ite ");
                if (status == QL_STATUS_OK) {
                    status = term_block_symbol(
                        encoder, side, side->returns.items[index].block);
                }
                if (status == QL_STATUS_OK) {
                    status = term_add(encoder, " ");
                }
                if (status == QL_STATUS_OK) {
                    status = term_add(encoder,
                                      side_value_symbol(
                                          side,
                                          side->returns.items[index].value));
                }
                if (status == QL_STATUS_OK) {
                    status = term_add(encoder, " ");
                }
            }
            if (status == QL_STATUS_OK) {
                status = term_add(
                    encoder,
                    side_value_symbol(
                        side,
                        side->returns.items[side->returns.count - 1u].value));
            }
            for (index = 0u; index + 1u < side->returns.count &&
                             status == QL_STATUS_OK;
                 ++index) {
                status = term_add(encoder, ")");
            }
        }
        if (status == QL_STATUS_OK) {
            status = emit_sorted(encoder, symbol, side->return_kind,
                                 side->return_width);
        }
        if (status != QL_STATUS_OK) {
            return status;
        }
    }

    (void)snprintf(symbol, sizeof(symbol), "%c_trap_code", side->prefix);
    buffer_reset(&encoder->term);
    status = QL_STATUS_OK;
    for (index = 0u; index < side->traps.count && status == QL_STATUS_OK;
         ++index) {
        status = term_add(encoder, "(ite ");
        if (status == QL_STATUS_OK) {
            status = term_block_symbol(encoder, side,
                                       side->traps.items[index].block);
        }
        if (status == QL_STATUS_OK) {
            status = term_addf(encoder, " (_ bv%llu 64) ",
                               (unsigned long long)
                                   side->traps.items[index].code);
        }
    }
    if (status == QL_STATUS_OK) {
        status = term_add(encoder, "(_ bv0 64)");
    }
    for (index = 0u; index < side->traps.count && status == QL_STATUS_OK;
         ++index) {
        status = term_add(encoder, ")");
    }
    if (status == QL_STATUS_OK) {
        status = emit_bv(encoder, symbol, 64u);
    }
    return status;
}

static ql_status encode_side(product_encoder *encoder, product_side *side,
                             const ql_allocator *allocator) {
    size_t order_index;
    ql_status status;

    status = encode_constants(encoder, side);
    if (status != QL_STATUS_OK) {
        return status;
    }
    for (order_index = 0u; order_index < side->view.block_count;
         ++order_index) {
        const ql_ir_block_id block_id = side->order[order_index];
        ql_ir_block_view_v1 block;
        size_t index;

        memset(&block, 0, sizeof(block));
        block.struct_size = sizeof(block);
        status = ql_ir_block_at(side->ir, block_id, &block, encoder->error);
        if (status != QL_STATUS_OK) {
            return status;
        }
        status = encode_block_reach(encoder, side, block_id);
        if (status != QL_STATUS_OK) {
            return status;
        }
        for (index = 0u; index < block.instruction_count; ++index) {
            ql_ir_instruction_view_v1 instruction;
            memset(&instruction, 0, sizeof(instruction));
            instruction.struct_size = sizeof(instruction);
            status = ql_ir_instruction_at(side->ir, block.instructions[index],
                                          &instruction, encoder->error);
            if (status != QL_STATUS_OK) {
                return status;
            }
            status = encode_instruction(encoder, side, &instruction, block_id,
                                        allocator);
            if (status != QL_STATUS_OK) {
                return status;
            }
        }
        status = encode_block_edges(encoder, side, &block);
        if (status != QL_STATUS_OK) {
            return status;
        }
        switch (block.terminator.kind) {
        case QL_IR_TERMINATOR_RETURN:
            status = site_list_add(&side->returns, allocator, block_id,
                                   block.terminator.return_value, 0u,
                                   encoder->error);
            break;
        case QL_IR_TERMINATOR_TRAP:
            status = site_list_add(&side->traps, allocator, block_id,
                                   QL_IR_INVALID_VALUE_ID,
                                   block.terminator.code, encoder->error);
            break;
        case QL_IR_TERMINATOR_UNDEFINED_BEHAVIOR:
            status = site_list_add(&side->undefined, allocator, block_id,
                                   QL_IR_INVALID_VALUE_ID, 0u,
                                   encoder->error);
            break;
        case QL_IR_TERMINATOR_DIVERGE:
            status = site_list_add(&side->diverges, allocator, block_id,
                                   QL_IR_INVALID_VALUE_ID, 0u,
                                   encoder->error);
            break;
        default:
            status = QL_STATUS_OK;
            break;
        }
        if (status != QL_STATUS_OK) {
            return status;
        }
    }
    return encode_side_aggregates(encoder, side);
}

/* --- Precondition --------------------------------------------------------- */

static ql_status encode_precondition_node(product_encoder *encoder,
                                          const ql_precondition *precondition,
                                          uint32_t node_index) {
    ql_precondition_node_view_v1 node;
    const char *name = NULL;
    size_t index;
    ql_status status;

    memset(&node, 0, sizeof(node));
    node.struct_size = sizeof(node);
    status = ql_precondition_node_at(precondition, node_index, &node,
                                     encoder->error);
    if (status != QL_STATUS_OK) {
        return status;
    }
    switch (node.kind) {
    case QL_PRECONDITION_NODE_BOOL:
        return term_add(encoder, node.boolean_value != 0u ? "true" : "false");
    case QL_PRECONDITION_NODE_ARGUMENT:
        if ((size_t)node.argument_index >= encoder->input_count) {
            ql_error_set(encoder->error, QL_STATUS_TYPE_MISMATCH,
                         "precondition references argument %u outside the signature",
                         node.argument_index);
            return QL_STATUS_TYPE_MISMATCH;
        }
        return term_add(encoder,
                        encoder->inputs[node.argument_index].symbol);
    case QL_PRECONDITION_NODE_INTEGER:
        /* SMT-LIB has no negative numeral, so a canonical negative decimal is
           negated modulo the same width instead of being reformatted. */
        if (node.integer_value_size != 0u && node.integer_value[0] == '-') {
            status = term_add(encoder, "(bvneg (_ bv");
            if (status == QL_STATUS_OK) {
                status = term_add_bytes(encoder, node.integer_value + 1,
                                        node.integer_value_size - 1u);
            }
            if (status == QL_STATUS_OK) {
                status = term_addf(encoder, " %u))", node.bit_width);
            }
            return status;
        }
        status = term_add(encoder, "(_ bv");
        if (status == QL_STATUS_OK) {
            status = term_add_bytes(encoder, node.integer_value,
                                    node.integer_value_size);
        }
        if (status == QL_STATUS_OK) {
            status = term_addf(encoder, " %u)", node.bit_width);
        }
        return status;
    case QL_PRECONDITION_NODE_NOT:
        name = "not";
        break;
    case QL_PRECONDITION_NODE_AND:
        name = "and";
        break;
    case QL_PRECONDITION_NODE_OR:
        name = "or";
        break;
    case QL_PRECONDITION_NODE_IMPLIES:
        name = "=>";
        break;
    case QL_PRECONDITION_NODE_EQUAL:
        name = "=";
        break;
    case QL_PRECONDITION_NODE_NOT_EQUAL:
        name = "distinct";
        break;
    case QL_PRECONDITION_NODE_SIGNED_LESS:
        name = "bvslt";
        break;
    case QL_PRECONDITION_NODE_SIGNED_LESS_EQUAL:
        name = "bvsle";
        break;
    case QL_PRECONDITION_NODE_SIGNED_GREATER:
        name = "bvsgt";
        break;
    case QL_PRECONDITION_NODE_SIGNED_GREATER_EQUAL:
        name = "bvsge";
        break;
    case QL_PRECONDITION_NODE_UNSIGNED_LESS:
        name = "bvult";
        break;
    case QL_PRECONDITION_NODE_UNSIGNED_LESS_EQUAL:
        name = "bvule";
        break;
    case QL_PRECONDITION_NODE_UNSIGNED_GREATER:
        name = "bvugt";
        break;
    case QL_PRECONDITION_NODE_UNSIGNED_GREATER_EQUAL:
        name = "bvuge";
        break;
    case QL_PRECONDITION_NODE_SIGNED_ADD:
    case QL_PRECONDITION_NODE_UNSIGNED_ADD:
        name = "bvadd";
        break;
    case QL_PRECONDITION_NODE_SIGNED_SUBTRACT:
    case QL_PRECONDITION_NODE_UNSIGNED_SUBTRACT:
        name = "bvsub";
        break;
    case QL_PRECONDITION_NODE_SIGNED_MULTIPLY:
    case QL_PRECONDITION_NODE_UNSIGNED_MULTIPLY:
        name = "bvmul";
        break;
    default:
        ql_error_set(encoder->error, QL_STATUS_TYPE_MISMATCH,
                     "precondition node kind %u needs a memory model that the scalar miter does not have",
                     (unsigned)node.kind);
        return QL_STATUS_TYPE_MISMATCH;
    }

    status = term_add(encoder, "(");
    if (status == QL_STATUS_OK) {
        status = term_add(encoder, name);
    }
    for (index = 0u; index < node.child_count && status == QL_STATUS_OK;
         ++index) {
        status = term_add(encoder, " ");
        if (status == QL_STATUS_OK) {
            status = encode_precondition_node(encoder, precondition,
                                              node.children[index]);
        }
    }
    if (status == QL_STATUS_OK) {
        status = term_add(encoder, ")");
    }
    return status;
}

static ql_status encode_precondition(product_encoder *encoder,
                                     const ql_source_signature *signature,
                                     const ql_semantic_contract_v1 *contract) {
    ql_signature_argument_v1 storage[QL_SOURCE_SIGNATURE_MAX_ARGUMENTS];
    ql_signature_view_v1 signature_view;
    ql_precondition_view_v1 precondition_view;
    ql_precondition *precondition = NULL;
    ql_status status;

    memset(&signature_view, 0, sizeof(signature_view));
    status = ql_source_signature_precondition_view(
        signature, &signature_view, storage,
        QL_SOURCE_SIGNATURE_MAX_ARGUMENTS, encoder->error);
    if (status != QL_STATUS_OK) {
        return status;
    }
    status = ql_precondition_parse(
        encoder->allocator, contract->precondition_json,
        contract->precondition_json_size, &signature_view, &precondition,
        encoder->error);
    if (status != QL_STATUS_OK) {
        return status;
    }
    memset(&precondition_view, 0, sizeof(precondition_view));
    precondition_view.struct_size = sizeof(precondition_view);
    status = ql_precondition_get_view(precondition, &precondition_view,
                                      encoder->error);
    if (status == QL_STATUS_OK) {
        buffer_reset(&encoder->term);
        status = encode_precondition_node(encoder, precondition,
                                          precondition_view.root_node);
    }
    if (status == QL_STATUS_OK) {
        status = emit_bool(encoder, QL_PRODUCT_PRECONDITION_SYMBOL);
    }
    ql_precondition_destroy(precondition);
    return status;
}

/* --- Relation obligations ------------------------------------------------- */

/* Comparing the return-value projection means comparing whether a normal
   return happened at all, then the value. A trapping or diverging execution
   produces no return value, which is a different observation from any value. */
static ql_status encode_observation_equality(
    product_encoder *encoder, const ql_semantic_contract_v1 *contract,
    int compare_return_value) {
    ql_status status;

    buffer_reset(&encoder->term);
    status = term_add(encoder, "(and true");
    if (status == QL_STATUS_OK &&
        (contract->observations & QL_OBSERVE_RETURN_VALUE) != 0u) {
        status = term_add(encoder, " (= l_returns r_returns)");
        if (status == QL_STATUS_OK && compare_return_value) {
            status = term_add(
                encoder,
                " (=> l_returns (= l_return_value r_return_value))");
        }
    }
    if (status == QL_STATUS_OK &&
        (contract->observations & QL_OBSERVE_TERMINATION) != 0u) {
        status = term_add(encoder, " (= l_terminates r_terminates)");
    }
    if (status == QL_STATUS_OK &&
        (contract->observations & QL_OBSERVE_TRAPS) != 0u) {
        status = term_add(encoder, " (= l_traps r_traps)");
        if (status == QL_STATUS_OK) {
            status = term_add(
                encoder, " (=> l_traps (= l_trap_code r_trap_code))");
        }
    }
    if (status == QL_STATUS_OK) {
        status = term_add(encoder, ")");
    }
    if (status != QL_STATUS_OK) {
        return status;
    }
    return emit_bool(encoder, QL_PRODUCT_OBSERVATION_SYMBOL);
}

/* The comparison domain. UNSAT here means the precondition and UB policy
   leave nothing to compare, so an UNSAT miter would be vacuous. */
static ql_status encode_domain(product_encoder *encoder,
                               const ql_semantic_contract_v1 *contract) {
    const char *definedness;
    ql_status status;

    switch (contract->ub_policy) {
    case QL_UB_LANGUAGE_REFINEMENT:
        switch (contract->relation) {
        case QL_RELATION_LEFT_REFINES_RIGHT:
            definedness = "r_defined";
            break;
        case QL_RELATION_RIGHT_REFINES_LEFT:
            definedness = "l_defined";
            break;
        default:
            definedness = "(or l_defined r_defined)";
            break;
        }
        break;
    default:
        definedness = "(and l_defined r_defined)";
        break;
    }
    buffer_reset(&encoder->term);
    status = term_add(encoder, "(and " QL_PRODUCT_PRECONDITION_SYMBOL " ");
    if (status == QL_STATUS_OK) {
        status = term_add(encoder, definedness);
    }
    if (status == QL_STATUS_OK) {
        status = term_add(encoder, ")");
    }
    if (status != QL_STATUS_OK) {
        return status;
    }
    return emit_bool(encoder, QL_PRODUCT_DOMAIN_SYMBOL);
}

/* Every policy conjoins the observation obligation with both sides being
   defined, so a totalized SMT division or shift never reaches an observation
   claim on an input where the C semantics are undefined. */
static ql_status encode_violation(product_encoder *encoder,
                                  const ql_semantic_contract_v1 *contract) {
    const char *body;
    ql_status status;

    switch (contract->ub_policy) {
    case QL_UB_MUST_MATCH:
        body =
            "(or (not (= l_defined r_defined))"
            " (and l_defined r_defined (not " QL_PRODUCT_OBSERVATION_SYMBOL
            ")))";
        break;
    case QL_UB_LANGUAGE_REFINEMENT:
        switch (contract->relation) {
        case QL_RELATION_LEFT_REFINES_RIGHT:
            body =
                "(and r_defined (or (not l_defined) (not "
                QL_PRODUCT_OBSERVATION_SYMBOL ")))";
            break;
        case QL_RELATION_RIGHT_REFINES_LEFT:
            body =
                "(and l_defined (or (not r_defined) (not "
                QL_PRODUCT_OBSERVATION_SYMBOL ")))";
            break;
        default:
            body =
                "(or (not (= l_defined r_defined))"
                " (and l_defined r_defined (not "
                QL_PRODUCT_OBSERVATION_SYMBOL ")))";
            break;
        }
        break;
    default:
        body = "(and l_defined r_defined (not "
               QL_PRODUCT_OBSERVATION_SYMBOL "))";
        break;
    }
    buffer_reset(&encoder->term);
    status = term_add(encoder, "(and " QL_PRODUCT_PRECONDITION_SYMBOL " ");
    if (status == QL_STATUS_OK) {
        status = term_add(encoder, body);
    }
    if (status == QL_STATUS_OK) {
        status = term_add(encoder, ")");
    }
    if (status != QL_STATUS_OK) {
        return status;
    }
    return emit_bool(encoder, QL_PRODUCT_VIOLATION_SYMBOL);
}

/* --- Inputs --------------------------------------------------------------- */

static ql_status build_inputs(ql_product_query *query,
                              const ql_problem *problem,
                              const ql_source_signature *left_signature,
                              size_t argument_count, ql_error *error) {
    size_t index;
    ql_status status;

    if (argument_count == 0u) {
        return QL_STATUS_OK;
    }
    query->inputs = query->allocator.allocate(
        query->allocator.user_data, argument_count * sizeof(*query->inputs));
    query->input_symbols = query->allocator.allocate(
        query->allocator.user_data,
        argument_count * QL_PRODUCT_SYMBOL_CAPACITY);
    if (query->inputs == NULL || query->input_symbols == NULL) {
        ql_error_set(error, QL_STATUS_OUT_OF_MEMORY, NULL);
        return QL_STATUS_OUT_OF_MEMORY;
    }
    memset(query->inputs, 0, argument_count * sizeof(*query->inputs));
    for (index = 0u; index < argument_count; ++index) {
        char *symbol =
            query->input_symbols + index * QL_PRODUCT_SYMBOL_CAPACITY;
        ql_problem_argument_binding_v1 binding;
        ql_source_type_v1 type;
        size_t search;
        int written;

        memset(&binding, 0, sizeof(binding));
        binding.left_index = UINT32_MAX;
        for (search = 0u; search < argument_count; ++search) {
            ql_problem_argument_binding_v1 candidate;
            status = ql_problem_argument_binding_at(problem, search,
                                                    &candidate, error);
            if (status != QL_STATUS_OK) {
                return status;
            }
            if ((size_t)candidate.left_index == index) {
                binding = candidate;
                break;
            }
        }
        if (binding.left_index == UINT32_MAX) {
            ql_error_set(error, QL_STATUS_INTERNAL_ERROR,
                         "argument correspondence is missing left index %zu",
                         index);
            return QL_STATUS_INTERNAL_ERROR;
        }
        status = ql_source_signature_argument_at(left_signature, index, &type,
                                                 error);
        if (status != QL_STATUS_OK) {
            return status;
        }
        written = snprintf(symbol, QL_PRODUCT_SYMBOL_CAPACITY, "in%zu", index);
        if (written < 0 || (size_t)written >= QL_PRODUCT_SYMBOL_CAPACITY) {
            ql_error_set(error, QL_STATUS_INTERNAL_ERROR,
                         "could not format a product input symbol");
            return QL_STATUS_INTERNAL_ERROR;
        }
        query->inputs[index].struct_size = sizeof(query->inputs[index]);
        query->inputs[index].index = (uint32_t)index;
        query->inputs[index].left_parameter = binding.left_index;
        query->inputs[index].right_parameter = binding.right_index;
        query->inputs[index].kind = type.kind;
        query->inputs[index].bit_width = type.bit_width;
        query->inputs[index].symbol = symbol;
    }
    return QL_STATUS_OK;
}

static ql_status declare_inputs(ql_smt2_builder *builder,
                                const ql_product_input_v1 *inputs,
                                size_t input_count, uint32_t *maximum_width,
                                ql_error *error) {
    size_t index;
    ql_status status;

    *maximum_width = 1u;
    for (index = 0u; index < input_count; ++index) {
        if (inputs[index].kind == QL_SOURCE_TYPE_BOOL) {
            status = ql_smt2_builder_declare_bool(builder,
                                                  inputs[index].symbol,
                                                  error);
        } else if (inputs[index].kind == QL_SOURCE_TYPE_SIGNED_INTEGER ||
                   inputs[index].kind == QL_SOURCE_TYPE_UNSIGNED_INTEGER) {
            if (inputs[index].bit_width > *maximum_width) {
                *maximum_width = inputs[index].bit_width;
            }
            status = ql_smt2_builder_declare_bv(builder, inputs[index].symbol,
                                                inputs[index].bit_width,
                                                error);
        } else {
            ql_error_set(error, QL_STATUS_TYPE_MISMATCH,
                         "argument %zu is a pointer, which the scalar miter does not model",
                         index);
            return QL_STATUS_TYPE_MISMATCH;
        }
        if (status != QL_STATUS_OK) {
            return status;
        }
    }
    return QL_STATUS_OK;
}

/* --- Public API ----------------------------------------------------------- */

static const ql_allocator *select_allocator(const ql_allocator *allocator) {
    return allocator == NULL ? ql_default_allocator() : allocator;
}

void QL_CALL ql_product_query_destroy(ql_product_query *query) {
    ql_allocator allocator;

    if (query == NULL) {
        return;
    }
    allocator = query->allocator;
    ql_artifact_release(query->prefix);
    ql_artifact_release(query->violation);
    ql_artifact_release(query->domain);
    allocator.deallocate(allocator.user_data, query->inputs);
    allocator.deallocate(allocator.user_data, query->input_symbols);
    allocator.deallocate(allocator.user_data, query);
}

ql_status QL_CALL ql_product_query_build(const ql_allocator *allocator,
                                         const ql_problem *problem,
                                         const ql_ir *left_ir,
                                         const ql_ir *right_ir,
                                         ql_product_query **output,
                                         ql_error *error) {
    const ql_allocator *selected = select_allocator(allocator);
    ql_product_query *query = NULL;
    ql_problem_view_v2 problem_view;
    ql_source_signature *left_signature = NULL;
    ql_source_signature *right_signature = NULL;
    ql_source_signature_view_v1 left_signature_view;
    product_encoder encoder;
    product_side left;
    product_side right;
    ql_artifact_view artifact_view;
    uint32_t maximum_width = 1u;

    ql_status status;

    if (output == NULL || problem == NULL || left_ir == NULL ||
        right_ir == NULL) {
        ql_error_set(error, QL_STATUS_INVALID_ARGUMENT,
                     "problem, both IR functions, and a query output are required");
        return QL_STATUS_INVALID_ARGUMENT;
    }
    *output = NULL;
    if (!ql_allocator_is_valid(selected)) {
        ql_error_set(error, QL_STATUS_INVALID_ARGUMENT, "invalid allocator");
        return QL_STATUS_INVALID_ARGUMENT;
    }
    memset(&problem_view, 0, sizeof(problem_view));
    problem_view.struct_size = sizeof(problem_view);
    status = ql_problem_get_view_v2(problem, &problem_view, error);
    if (status != QL_STATUS_OK) {
        return status;
    }
    memset(&left, 0, sizeof(left));
    memset(&right, 0, sizeof(right));
    memset(&encoder, 0, sizeof(encoder));

    query = selected->allocate(selected->user_data, sizeof(*query));
    if (query == NULL) {
        ql_error_set(error, QL_STATUS_OUT_OF_MEMORY, NULL);
        return QL_STATUS_OUT_OF_MEMORY;
    }
    memset(query, 0, sizeof(*query));
    query->allocator = *selected;

    status = ql_source_signature_open(
        selected, ql_problem_left_signature_artifact(problem),
        &left_signature, error);
    if (status == QL_STATUS_OK) {
        status = ql_source_signature_open(
            selected, ql_problem_right_signature_artifact(problem),
            &right_signature, error);
    }
    if (status == QL_STATUS_OK) {
        status = ql_source_signature_bind_ir(left_signature, left_ir, error);
    }
    if (status == QL_STATUS_OK) {
        status = ql_source_signature_bind_ir(right_signature, right_ir,
                                             error);
    }
    if (status != QL_STATUS_OK) {
        goto cleanup;
    }
    memset(&left_signature_view, 0, sizeof(left_signature_view));
    left_signature_view.struct_size = sizeof(left_signature_view);
    status = ql_source_signature_get_view(left_signature,
                                          &left_signature_view, error);
    if (status != QL_STATUS_OK) {
        goto cleanup;
    }

    status = build_inputs(query, problem, left_signature,
                          left_signature_view.argument_count, error);
    if (status != QL_STATUS_OK) {
        goto cleanup;
    }
    query->view.input_count = left_signature_view.argument_count;

    encoder.allocator = selected;
    encoder.error = error;
    encoder.inputs = query->inputs;
    encoder.input_count = query->view.input_count;
    buffer_init(&encoder.term, selected);
    status = ql_smt2_builder_create(selected, QL_SOLVER_LOGIC_QF_BV,
                                    &encoder.builder, error);
    if (status != QL_STATUS_OK) {
        goto cleanup;
    }

    status = side_prepare(&left, left_ir, 'l', selected, query->inputs,
                          query->view.input_count, error);
    if (status == QL_STATUS_OK) {
        status = side_prepare(&right, right_ir, 'r', selected, query->inputs,
                              query->view.input_count, error);
    }
    if (status != QL_STATUS_OK) {
        goto cleanup;
    }

    status = check_ir_fragment(left_ir, &left.view, "left", error);
    if (status == QL_STATUS_OK) {
        status = check_ir_fragment(right_ir, &right.view, "right", error);
    }
    if (status != QL_STATUS_OK) {
        goto cleanup;
    }
    if (left.return_kind != right.return_kind ||
        left.return_width != right.return_width) {
        ql_error_set(error, QL_STATUS_TYPE_MISMATCH,
                     "the two IR functions do not share a return type");
        status = QL_STATUS_TYPE_MISMATCH;
        goto cleanup;
    }

    status = declare_inputs(encoder.builder, query->inputs,
                            query->view.input_count, &maximum_width, error);
    if (status == QL_STATUS_OK) {
        status = encode_precondition(&encoder, left_signature,
                                     &problem_view.contract);
    }
    if (status == QL_STATUS_OK) {
        status = encode_side(&encoder, &left, selected);
    }
    if (status == QL_STATUS_OK) {
        status = encode_side(&encoder, &right, selected);
    }
    if (status == QL_STATUS_OK) {
        status = encode_observation_equality(
            &encoder, &problem_view.contract,
            left.return_kind != QL_IR_TYPE_VOID);
    }
    if (status == QL_STATUS_OK) {
        status = encode_domain(&encoder, &problem_view.contract);
    }
    if (status == QL_STATUS_OK) {
        status = encode_violation(&encoder, &problem_view.contract);
    }
    if (status == QL_STATUS_OK) {
        status = ql_smt2_builder_build(encoder.builder, &query->prefix,
                                       error);
    }
    if (status == QL_STATUS_OK) {
        status = ql_artifact_create(
            selected, QL_ARTIFACT_KIND_SMTLIB2, QL_SMTLIB2_SCHEMA_VERSION,
            product_violation_assertion,
            sizeof(product_violation_assertion) - 1u, &query->violation,
            error);
    }
    if (status == QL_STATUS_OK) {
        status = ql_artifact_create(
            selected, QL_ARTIFACT_KIND_SMTLIB2, QL_SMTLIB2_SCHEMA_VERSION,
            product_domain_assertion, sizeof(product_domain_assertion) - 1u,
            &query->domain, error);
    }
    if (status != QL_STATUS_OK) {
        goto cleanup;
    }

    query->view.struct_size = sizeof(query->view);
    query->view.schema_version = QL_PRODUCT_SCHEMA_VERSION;
    query->view.relation = problem_view.contract.relation;
    query->view.ub_policy = problem_view.contract.ub_policy;
    query->view.covered_observations = problem_view.contract.observations;
    query->view.logic = QL_SOLVER_LOGIC_QF_BV;
    query->view.return_type_kind = left.return_kind;
    query->view.return_bit_width = left.return_width;
    query->view.problem_digest = problem_view.artifact_digest;
    if (left.return_kind == QL_IR_TYPE_BIT_VECTOR &&
        left.return_width > maximum_width) {
        maximum_width = left.return_width;
    }
    if (maximum_width < 64u) {
        /* The trap-code channel is always a 64-bit vector. */
        maximum_width = 64u;
    }
    query->view.maximum_bv_width = maximum_width;

    memset(&artifact_view, 0, sizeof(artifact_view));
    artifact_view.struct_size = sizeof(artifact_view);
    status = ql_artifact_get_view(query->prefix, &artifact_view, error);
    if (status == QL_STATUS_OK) {
        query->view.prefix_digest = artifact_view.digest;
        status = ql_artifact_get_view(query->violation, &artifact_view,
                                      error);
    }
    if (status == QL_STATUS_OK) {
        query->view.violation_digest = artifact_view.digest;
        status = ql_artifact_get_view(query->domain, &artifact_view, error);
    }
    if (status == QL_STATUS_OK) {
        query->view.domain_digest = artifact_view.digest;
    }

cleanup:
    side_dispose(&left, selected);
    side_dispose(&right, selected);
    buffer_dispose(&encoder.term);
    ql_smt2_builder_destroy(encoder.builder);
    ql_source_signature_release(left_signature);
    ql_source_signature_release(right_signature);
    if (status != QL_STATUS_OK) {
        ql_product_query_destroy(query);
        return status;
    }
    *output = query;
    ql_error_clear(error);
    return QL_STATUS_OK;
}

ql_status QL_CALL ql_product_query_get_view(const ql_product_query *query,
                                            ql_product_query_view_v1 *view,
                                            ql_error *error) {
    size_t caller_size;

    if (query == NULL || view == NULL) {
        ql_error_set(error, QL_STATUS_INVALID_ARGUMENT,
                     "product query and view are required");
        return QL_STATUS_INVALID_ARGUMENT;
    }
    caller_size = view->struct_size;
    if (caller_size != 0u && caller_size < sizeof(*view)) {
        ql_error_set(error, QL_STATUS_ABI_MISMATCH,
                     "product query view structure is too small");
        return QL_STATUS_ABI_MISMATCH;
    }
    *view = query->view;
    ql_error_clear(error);
    return QL_STATUS_OK;
}

ql_status QL_CALL ql_product_query_input_at(const ql_product_query *query,
                                            size_t index,
                                            ql_product_input_v1 *output,
                                            ql_error *error) {
    if (query == NULL || output == NULL) {
        ql_error_set(error, QL_STATUS_INVALID_ARGUMENT,
                     "product query and input output are required");
        return QL_STATUS_INVALID_ARGUMENT;
    }
    if (index >= query->view.input_count) {
        ql_error_set(error, QL_STATUS_NOT_FOUND,
                     "product query has no input %zu", index);
        return QL_STATUS_NOT_FOUND;
    }
    *output = query->inputs[index];
    ql_error_clear(error);
    return QL_STATUS_OK;
}

const ql_artifact *QL_CALL ql_product_query_prefix_artifact(
    const ql_product_query *query) {
    return query == NULL ? NULL : query->prefix;
}

const ql_artifact *QL_CALL ql_product_query_violation_artifact(
    const ql_product_query *query) {
    return query == NULL ? NULL : query->violation;
}

const ql_artifact *QL_CALL ql_product_query_domain_artifact(
    const ql_product_query *query) {
    return query == NULL ? NULL : query->domain;
}
