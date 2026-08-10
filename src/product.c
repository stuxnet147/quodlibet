#include "quodlibet/product.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "quodlibet/precondition.h"

/* Compiles to nothing unless QL_STAGE_TIMING is defined. The hooks below
   bracket the four points where the accumulated encoding is rendered into
   SMT-LIB text and hashed into an artifact, which is the only place in this
   file where serialisation is separable from the encoding that feeds it.
   W8 added them and owns them. */
#include "stage_timer.h"

#define QL_PRODUCT_SYMBOL_CAPACITY 40u
#define QL_PRODUCT_MAX_BV_WIDTH 1024u
/* The ASM2C_GNU_V1 profile is a flat 64-bit address space of bytes. */
#define QL_PRODUCT_ADDRESS_WIDTH 64u
#define QL_PRODUCT_BYTE_WIDTH 8u
/* An access wider than this is left to a later slice rather than encoded with
   a byte count the concrete replay cannot reproduce. */
#define QL_PRODUCT_MAX_ACCESS_WIDTH 128u
/* The size bound the search-only violation query imposes so a model can be
   materialized and replayed. It matches QL_REPLAY_MAX_OBJECT_BYTES. */
#define QL_PRODUCT_REPLAYABLE_OBJECT_BYTES UINT64_C(4096)

#define QL_PRODUCT_PRECONDITION_SYMBOL "quodlibet_precondition"
#define QL_PRODUCT_OBSERVATION_SYMBOL "quodlibet_observation_equal"
#define QL_PRODUCT_DOMAIN_SYMBOL "quodlibet_domain"
#define QL_PRODUCT_VIOLATION_SYMBOL "quodlibet_violation"
#define QL_PRODUCT_ASSUMPTION_SYMBOL "quodlibet_assumptions"
#define QL_PRODUCT_MEMORY_SYMBOL "mem0"
#define QL_PRODUCT_TRACE_SYMBOL "trace0"
/* An event trace has no bit-width of its own. It is carried as a bit-vector
   because what the encoding needs of it is equality, and because congruence
   -- not the width -- is what makes two traces provably the same. Two traces
   the solver leaves equal when the programs would not is a proof this
   encoding will not find, never one it will accept. */
#define QL_PRODUCT_TRACE_WIDTH 64u
/* Congruence is stated pairwise over call sites, so it grows with the square
   of how many there are. Past this the query is refused rather than built at
   a size nothing will answer. */
#define QL_PRODUCT_MAX_CALL_SITES 32u
/* The lowering already refuses a call with more arguments than this. */
#define QL_PRODUCT_MAX_CALL_ARGUMENTS 32u
/* Results are the outgoing state, an optional return value, and any
   call-specific auxiliary values. Keep them bounded for the same reason as
   arguments, but do not silently drop results past the old three-value C
   lowering convention: every result participates in call congruence. */
#define QL_PRODUCT_MAX_CALL_RESULTS 64u
/* One free address constant states the whole final-memory comparison. In the
   violation query a free constant is existential, which is exactly "some
   address differs"; in the same query answered UNSAT it is universal, which is
   exactly "every address agrees". One constant is precise in both directions,
   and no quantifier enters the logic. */
#define QL_PRODUCT_PROBE_SYMBOL "mem_probe"

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
  /* The terminal memory a return carries, or the invalid sentinel. */
  ql_ir_value_id memory;
  /* The terminal event trace a return carries, or the invalid sentinel. */
  ql_ir_value_id trace;
  uint64_t code;
} product_site;

typedef struct product_site_list {
  product_site *items;
  size_t count;
  size_t capacity;
} product_site_list;

/* One external call, on one side. The operands are the incoming trace, the
   incoming memory, and the arguments; the results are whichever of the
   outgoing trace, outgoing memory, and returned value the call produces. */
typedef struct product_call {
  const char *symbol;
  size_t symbol_size;
  char prefix;
  ql_ir_block_id block;
  ql_ir_value_id operands[QL_PRODUCT_MAX_CALL_ARGUMENTS + 2u];
  size_t operand_count;
  ql_ir_value_id results[QL_PRODUCT_MAX_CALL_RESULTS];
  size_t result_count;
} product_call;

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
  /* Pointee width, in bits, for every pointer-typed value. Zero elsewhere. */
  uint32_t *value_element_widths;
  product_edge *edges;
  size_t edge_count;
  ql_ir_block_id *order;
  product_site_list guards;
  product_site_list returns;
  product_site_list traps;
  product_site_list undefined;
  product_site_list diverges;
  product_site_list assumes;
  product_call calls[QL_PRODUCT_MAX_CALL_SITES];
  size_t call_count;
  ql_ir_type_kind return_kind;
  uint32_t return_width;
  /* One shared object index per pointer parameter of this side, in this
     side's own parameter order. */
  const uint32_t *object_map;
  size_t argument_count;
  size_t object_count;
} product_side;

/* One storage region, named once and bound on both sides. */
typedef struct product_object {
  char base_symbol[QL_PRODUCT_SYMBOL_CAPACITY];
  char size_symbol[QL_PRODUCT_SYMBOL_CAPACITY];
  uint32_t left_base_parameter;
  uint32_t right_base_parameter;
} product_object;

typedef struct product_encoder {
  const ql_allocator *allocator;
  ql_smt2_builder *builder;
  product_buffer term;
  const ql_product_input_v1 *inputs;
  size_t input_count;
  const product_object *objects;
  size_t object_count;
  ql_error *error;
} product_encoder;

struct ql_product_query {
  ql_allocator allocator;
  ql_product_query_view_v1 view;
  ql_product_input_v1 *inputs;
  char *input_symbols;
  product_object *objects;
  size_t object_count;
  ql_artifact *prefix;
  ql_artifact *violation;
  ql_artifact *bounded_violation;
  ql_artifact *domain;
};

/* --- Text buffer ---------------------------------------------------------- */

static void buffer_init(product_buffer *buffer, const ql_allocator *allocator) {
  memset(buffer, 0, sizeof(*buffer));
  buffer->allocator = *allocator;
}

static void buffer_dispose(product_buffer *buffer) {
  if (buffer->data != NULL) {
    buffer->allocator.deallocate(buffer->allocator.user_data, buffer->data);
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
    allocation = buffer->allocator.reallocate(buffer->allocator.user_data,
                                              buffer->data, capacity);
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
  char scratch[256];
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
    const size_t capacity = list->capacity == 0u ? 8u : list->capacity * 2u;
    void *allocation = allocator->reallocate(allocator->user_data, list->items,
                                             capacity * sizeof(*list->items));
    if (allocation == NULL) {
      ql_error_set(error, QL_STATUS_OUT_OF_MEMORY, NULL);
      return QL_STATUS_OUT_OF_MEMORY;
    }
    list->items = (product_site *)allocation;
    list->capacity = capacity;
  }
  list->items[list->count].block = block;
  list->items[list->count].value = value;
  list->items[list->count].memory = QL_IR_INVALID_VALUE_ID;
  list->items[list->count].trace = QL_IR_INVALID_VALUE_ID;
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
  case QL_IR_OPCODE_PTR_ADD:
  case QL_IR_OPCODE_PTR_TO_BV:
  case QL_IR_OPCODE_BV_TO_PTR:
  case QL_IR_OPCODE_LOAD:
  case QL_IR_OPCODE_STORE:
  case QL_IR_OPCODE_ASSUME:
  case QL_IR_OPCODE_MEMORY_IMAGE:
  case QL_IR_OPCODE_CALL:
    return 1;
  default:
    return 0;
  }
}

static int opcode_is_memory_access(ql_ir_opcode opcode) {
  return opcode == QL_IR_OPCODE_LOAD || opcode == QL_IR_OPCODE_STORE;
}

/* Refuses everything the scalar miter cannot state, so no observation axis is
   ever silently dropped. */
static ql_status check_ir_fragment(const ql_ir *ir, const ql_ir_view_v1 *view,
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
        type.kind != QL_IR_TYPE_BIT_VECTOR && type.kind != QL_IR_TYPE_POINTER &&
        type.kind != QL_IR_TYPE_MEMORY && type.kind != QL_IR_TYPE_EVENT_TRACE) {
      ql_error_set(error, QL_STATUS_TYPE_MISMATCH,
                   "%s IR uses a type the miter cannot state; floats and "
                   "aggregates are outside this fragment",
                   side);
      return QL_STATUS_TYPE_MISMATCH;
    }
    if (type.kind == QL_IR_TYPE_POINTER &&
        type.bit_width != QL_PRODUCT_ADDRESS_WIDTH) {
      ql_error_set(error, QL_STATUS_TYPE_MISMATCH,
                   "%s IR uses a %u-bit pointer; the flat memory model this "
                   "miter encodes is a %u-bit address space",
                   side, type.bit_width, QL_PRODUCT_ADDRESS_WIDTH);
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
    /* A load or a store carries exactly the memory effect and nothing
       else. Every other effect names an axis this encoding has no term
       for, so it is refused instead of dropped. */
    if (instruction.opcode == QL_IR_OPCODE_CALL) {
      /* An external call is uninterpreted: it may read and write
         memory and it is itself observable. Those three effects
         together are the call this encoding states; any other effect
         bit names an axis it has no term for. */
      if (instruction.effects !=
          (QL_IR_EFFECT_CALL | QL_IR_EFFECT_MEMORY | QL_IR_EFFECT_IO)) {
        ql_error_set(error, QL_STATUS_TYPE_MISMATCH,
                     "%s IR call carries an effect beyond call, memory, and "
                     "I/O that this miter does not model",
                     side);
        return QL_STATUS_TYPE_MISMATCH;
      }
      if (instruction.symbol_size == 0u) {
        ql_error_set(error, QL_STATUS_TYPE_MISMATCH,
                     "%s IR call names no callee, so nothing says which calls "
                     "correspond",
                     side);
        return QL_STATUS_TYPE_MISMATCH;
      }
      if (instruction.operand_count > QL_PRODUCT_MAX_CALL_ARGUMENTS + 2u) {
        ql_error_set(error, QL_STATUS_TYPE_MISMATCH,
                     "%s IR call passes more arguments than this miter states",
                     side);
        return QL_STATUS_TYPE_MISMATCH;
      }
      if (instruction.result_count > QL_PRODUCT_MAX_CALL_RESULTS) {
        ql_error_set(error, QL_STATUS_TYPE_MISMATCH,
                     "%s IR call produces more results than this miter "
                     "states",
                     side);
        return QL_STATUS_TYPE_MISMATCH;
      }
      continue;
    }
    if (opcode_is_memory_access(instruction.opcode)) {
      if (instruction.effects != QL_IR_EFFECT_MEMORY) {
        ql_error_set(error, QL_STATUS_TYPE_MISMATCH,
                     "%s IR access carries a volatile, atomic, or I/O effect "
                     "that this miter does not model",
                     side);
        return QL_STATUS_TYPE_MISMATCH;
      }
      continue;
    }
    if (instruction.effects != QL_IR_EFFECT_NONE) {
      ql_error_set(error, QL_STATUS_TYPE_MISMATCH,
                   "%s IR has a call, volatile, atomic, or I/O effect that "
                   "this miter does not model",
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
    switch (block.terminator.kind) {
    case QL_IR_TERMINATOR_RETURN:
    case QL_IR_TERMINATOR_BRANCH:
    case QL_IR_TERMINATOR_COND_BRANCH:
    case QL_IR_TERMINATOR_TRAP:
    case QL_IR_TERMINATOR_UNDEFINED_BEHAVIOR:
    case QL_IR_TERMINATOR_DIVERGE:
      break;
    default:
      ql_error_set(
          error, QL_STATUS_TYPE_MISMATCH,
          "%s IR terminator %u has no observation axis in this contract", side,
          (unsigned)block.terminator.kind);
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
  allocator->deallocate(allocator->user_data, side->value_element_widths);
  allocator->deallocate(allocator->user_data, side->edges);
  allocator->deallocate(allocator->user_data, side->order);
  site_list_dispose(&side->guards, allocator);
  site_list_dispose(&side->returns, allocator);
  site_list_dispose(&side->traps, allocator);
  site_list_dispose(&side->undefined, allocator);
  site_list_dispose(&side->diverges, allocator);
  site_list_dispose(&side->assumes, allocator);
  memset(side, 0, sizeof(*side));
}

/* `element_width` is the pointee's bit width for a pointer type and zero
   otherwise; a load or a store needs it to know how many bytes it moves. */
static ql_status side_type_of(const ql_ir *ir, const ql_ir_view_v1 *view,
                              ql_ir_type_id type_id, ql_ir_type_kind *kind,
                              uint32_t *width, uint32_t *element_width,
                              ql_error *error) {
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
  *width = type.kind == QL_IR_TYPE_EVENT_TRACE ? QL_PRODUCT_TRACE_WIDTH
                                               : type.bit_width;
  if (element_width != NULL) {
    *element_width = 0u;
  }
  if (type.kind == QL_IR_TYPE_POINTER && element_width != NULL) {
    ql_ir_type_view_v1 element;
    memset(&element, 0, sizeof(element));
    element.struct_size = sizeof(element);
    if (type.element_type >= view->type_count ||
        ql_ir_type_at(ir, type.element_type, &element, error) != QL_STATUS_OK) {
      ql_error_set(error, QL_STATUS_TYPE_MISMATCH,
                   "IR pointer type %u has no readable pointee", type_id);
      return QL_STATUS_TYPE_MISMATCH;
    }
    *element_width = element.kind == QL_IR_TYPE_BOOL ? 1u : element.bit_width;
  }
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
                                  side->view.block_count * sizeof(*in_degree));
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
        side->edges[side->edge_count].to = block.terminator.false_target;
        ++side->edge_count;
      }
    }
  }
  return QL_STATUS_OK;
}

/* Parameters beyond the C arguments are the memory model's own: one memory
   value, an event trace when the body calls, then a base and a size per
   object in this side's own order. Every one of them is bound to a symbol both
   sides share, which is what makes the two functions run over the same objects
   and the same initial memory without either side describing the table to the
   other. */
static ql_status
side_parameter_symbol(product_side *side, const ql_product_input_v1 *inputs,
                      size_t input_count, const product_object *objects,
                      size_t object_count, size_t parameter_index,
                      ql_ir_type_kind kind, size_t *object_ordinal,
                      char *symbol, ql_error *error) {
  size_t input;
  size_t offset;
  size_t local;
  int written;

  if (parameter_index < input_count) {
    for (input = 0u; input < input_count; ++input) {
      const uint32_t ordinal = side->prefix == 'l'
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
  } else if (kind == QL_IR_TYPE_EVENT_TRACE) {
    /* Both sides start from the same history, exactly as they start from
       the same memory. Otherwise two identical call sequences would be
       free to produce different traces and nothing would ever cancel. */
    written = snprintf(symbol, QL_PRODUCT_SYMBOL_CAPACITY, "%s",
                       QL_PRODUCT_TRACE_SYMBOL);
  } else if (kind == QL_IR_TYPE_MEMORY) {
    written = snprintf(symbol, QL_PRODUCT_SYMBOL_CAPACITY, "%s",
                       QL_PRODUCT_MEMORY_SYMBOL);
  } else if (object_count == 0u) {
    ql_error_set(error, QL_STATUS_TYPE_MISMATCH,
                 "IR parameter %zu is outside the signature and no object "
                 "table explains it",
                 parameter_index);
    return QL_STATUS_TYPE_MISMATCH;
  } else {
    /* The object ordinal is counted as the walk goes rather than derived
       from the parameter index. What stands between the C arguments and
       the objects is not a fixed number of parameters: a body that calls
       threads an event trace beside the memory. */
    offset = (*object_ordinal)++;
    local = offset / 2u;
    if (local >= object_count) {
      ql_error_set(
          error, QL_STATUS_TYPE_MISMATCH,
          "IR parameter %zu is past the object table this signature implies",
          parameter_index);
      return QL_STATUS_TYPE_MISMATCH;
    }
    written = snprintf(symbol, QL_PRODUCT_SYMBOL_CAPACITY, "%s",
                       (offset % 2u) == 0u
                           ? objects[side->object_map[local]].base_symbol
                           : objects[side->object_map[local]].size_symbol);
  }
  if (written < 0 || (size_t)written >= QL_PRODUCT_SYMBOL_CAPACITY) {
    ql_error_set(error, QL_STATUS_INTERNAL_ERROR,
                 "could not format an IR value symbol");
    return QL_STATUS_INTERNAL_ERROR;
  }
  return QL_STATUS_OK;
}

/* A body that calls takes the incoming history as a parameter. That, and not
   the presence of a call instruction, is what says the two sides have a
   history to compare at all. */
static int ir_threads_a_trace(const ql_ir *ir, const ql_ir_view_v1 *given) {
  ql_ir_view_v1 owned;
  const ql_ir_view_v1 *view = given;
  size_t index;

  if (view == NULL) {
    ql_error ignored;
    memset(&owned, 0, sizeof(owned));
    owned.struct_size = sizeof(owned);
    if (ql_ir_get_view(ir, &owned, &ignored) != QL_STATUS_OK) {
      return 0;
    }
    view = &owned;
  }
  for (index = 0u; index < view->value_count; ++index) {
    ql_ir_value_view_v1 value;
    ql_ir_type_view_v1 type;
    ql_error ignored;
    memset(&value, 0, sizeof(value));
    value.struct_size = sizeof(value);
    if (ql_ir_value_at(ir, index, &value, &ignored) != QL_STATUS_OK) {
      return 0;
    }
    if (value.definition_kind != QL_IR_VALUE_PARAMETER) {
      continue;
    }
    memset(&type, 0, sizeof(type));
    type.struct_size = sizeof(type);
    if (ql_ir_type_at(ir, value.type, &type, &ignored) == QL_STATUS_OK &&
        type.kind == QL_IR_TYPE_EVENT_TRACE) {
      return 1;
    }
  }
  return 0;
}

static ql_status side_prepare(product_side *side, const ql_ir *ir, char prefix,
                              const ql_allocator *allocator,
                              const ql_product_input_v1 *inputs,
                              size_t input_count, const product_object *objects,
                              size_t object_count, const uint32_t *object_map,
                              ql_error *error) {
  size_t index;
  size_t parameter_index = 0u;
  size_t object_ordinal = 0u;
  int side_has_memory = 0;
  int side_has_trace = 0;
  ql_status status;

  memset(side, 0, sizeof(*side));
  side->ir = ir;
  side->prefix = prefix;
  side->argument_count = input_count;
  side->object_count = object_count;
  side->object_map = object_map;
  side->view.struct_size = sizeof(side->view);
  status = ql_ir_get_view(ir, &side->view, error);
  if (status != QL_STATUS_OK) {
    return status;
  }
  if (side->view.cfg_kind != QL_IR_CFG_ACYCLIC) {
    ql_error_set(error, QL_STATUS_TYPE_MISMATCH,
                 "cyclic IR is outside the loop-free SMT product");
    return QL_STATUS_TYPE_MISMATCH;
  }
  side->value_symbols =
      allocator->allocate(allocator->user_data,
                          side->view.value_count * QL_PRODUCT_SYMBOL_CAPACITY);
  side->value_kinds =
      allocator->allocate(allocator->user_data,
                          side->view.value_count * sizeof(*side->value_kinds));
  side->value_widths =
      allocator->allocate(allocator->user_data,
                          side->view.value_count * sizeof(*side->value_widths));
  side->value_element_widths = allocator->allocate(
      allocator->user_data,
      side->view.value_count * sizeof(*side->value_element_widths));
  side->order = allocator->allocate(
      allocator->user_data, side->view.block_count * sizeof(*side->order));
  if (side->value_symbols == NULL || side->value_kinds == NULL ||
      side->value_widths == NULL || side->value_element_widths == NULL ||
      side->order == NULL) {
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
                          &side->value_kinds[index], &side->value_widths[index],
                          &side->value_element_widths[index], error);
    if (status != QL_STATUS_OK) {
      return status;
    }
    if (value.definition_kind == QL_IR_VALUE_PARAMETER) {
      if (side->value_kinds[index] == QL_IR_TYPE_MEMORY) {
        side_has_memory = 1;
      } else if (side->value_kinds[index] == QL_IR_TYPE_EVENT_TRACE) {
        side_has_trace = 1;
      }
      status = side_parameter_symbol(
          side, inputs, input_count, objects, object_count, parameter_index,
          side->value_kinds[index], &object_ordinal, symbol, error);
      if (status != QL_STATUS_OK) {
        return status;
      }
      ++parameter_index;
      continue;
    }
    written = snprintf(symbol, QL_PRODUCT_SYMBOL_CAPACITY, "%c_v%u", prefix,
                       (unsigned)index);
    if (written < 0 || (size_t)written >= QL_PRODUCT_SYMBOL_CAPACITY) {
      ql_error_set(error, QL_STATUS_INTERNAL_ERROR,
                   "could not format an IR value symbol");
      return QL_STATUS_INTERNAL_ERROR;
    }
  }
  {
    /* The C arguments, then the observable states this side threads --
       the memory, and an event trace when it calls -- then two
       parameters per object. Counting the states rather than assuming
       one is what lets a calling body through. */
    const size_t states = (object_ordinal == 0u && !side_has_memory ? 0u : 1u) +
                          (side_has_trace ? 1u : 0u);
    const size_t expected = input_count + states + object_ordinal;
    if (parameter_index != expected) {
      ql_error_set(error, QL_STATUS_TYPE_MISMATCH,
                   "IR declares %zu parameters but the signature and its "
                   "object table imply %zu",
                   parameter_index, expected);
      return QL_STATUS_TYPE_MISMATCH;
    }
    if (object_ordinal != 2u * object_count) {
      ql_error_set(error, QL_STATUS_TYPE_MISMATCH,
                   "IR declares %zu object parameters for %zu objects",
                   object_ordinal, object_count);
      return QL_STATUS_TYPE_MISMATCH;
    }
  }
  status = side_type_of(ir, &side->view, side->view.return_type,
                        &side->return_kind, &side->return_width, NULL, error);
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
  return ql_smt2_builder_define_bool(
      encoder->builder, symbol, buffer_text(&encoder->term), encoder->error);
}

static ql_status emit_bv(product_encoder *encoder, const char *symbol,
                         uint32_t width) {
  return ql_smt2_builder_define_bv(encoder->builder, symbol, width,
                                   buffer_text(&encoder->term), encoder->error);
}

static ql_status emit_memory(product_encoder *encoder, const char *symbol) {
  return ql_smt2_builder_define_array(
      encoder->builder, symbol, QL_PRODUCT_ADDRESS_WIDTH, QL_PRODUCT_BYTE_WIDTH,
      buffer_text(&encoder->term), encoder->error);
}

/* A pointer is an address and nothing more under this profile, so it takes the
   same bit-vector sort as its width. */
static ql_status emit_sorted(product_encoder *encoder, const char *symbol,
                             ql_ir_type_kind kind, uint32_t width) {
  if (kind == QL_IR_TYPE_BOOL) {
    return emit_bool(encoder, symbol);
  }
  if (kind == QL_IR_TYPE_MEMORY) {
    return emit_memory(encoder, symbol);
  }
  return emit_bv(encoder, symbol, width);
}

static ql_status term_constant(product_encoder *encoder, ql_ir_type_kind kind,
                               uint32_t width, const void *data, size_t size) {
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
                                  const product_side *side, ql_ir_block_id from,
                                  ql_ir_block_id to) {
  return term_addf(encoder, "%c_e%u_%u", side->prefix, (unsigned)from,
                   (unsigned)to);
}

static ql_status term_block_symbol(product_encoder *encoder,
                                   const product_side *side,
                                   ql_ir_block_id block) {
  return term_addf(encoder, "%c_b%u", side->prefix, (unsigned)block);
}

/* `<pointer>` for byte zero and `(bvadd <pointer> (_ bvN 64))` beyond it. */
static ql_status term_byte_address(product_encoder *encoder,
                                   const product_side *side,
                                   ql_ir_value_id pointer, size_t byte) {
  ql_status status;

  if (byte == 0u) {
    return term_add(encoder, side_value_symbol((product_side *)side, pointer));
  }
  status = term_add(encoder, "(bvadd ");
  if (status == QL_STATUS_OK) {
    status =
        term_add(encoder, side_value_symbol((product_side *)side, pointer));
  }
  if (status == QL_STATUS_OK) {
    status =
        term_addf(encoder, " (_ bv%zu %u))", byte, QL_PRODUCT_ADDRESS_WIDTH);
  }
  return status;
}

/* Byte order is little-endian, the same order src/ir_interp.c reads and writes
   under this profile. The two must agree byte for byte or the replay would
   contradict the query it is checking. */
static ql_status check_access_width(product_encoder *encoder, uint32_t width,
                                    size_t *byte_count) {
  if (width == 0u || (width % QL_PRODUCT_BYTE_WIDTH) != 0u ||
      width > QL_PRODUCT_MAX_ACCESS_WIDTH) {
    ql_error_set(encoder->error, QL_STATUS_TYPE_MISMATCH,
                 "a %u-bit memory access is not a whole number of bytes this "
                 "miter encodes",
                 width);
    return QL_STATUS_TYPE_MISMATCH;
  }
  *byte_count = (size_t)(width / QL_PRODUCT_BYTE_WIDTH);
  return QL_STATUS_OK;
}

static ql_status term_selected_byte(product_encoder *encoder,
                                    product_side *side,
                                    const ql_ir_instruction_view_v1 *view,
                                    size_t byte) {
  ql_status status = term_add(encoder, "(select ");

  if (status == QL_STATUS_OK) {
    status = term_add(encoder, side_value_symbol(side, view->operands[0]));
  }
  if (status == QL_STATUS_OK) {
    status = term_add(encoder, " ");
  }
  if (status == QL_STATUS_OK) {
    status = term_byte_address(encoder, side, view->operands[1], byte);
  }
  if (status == QL_STATUS_OK) {
    status = term_add(encoder, ")");
  }
  return status;
}

/* concat puts the most significant byte first, and under little-endian that is
   the byte at the highest address. */
/* An image is a conjunction over its bytes: each address in the run holds the
   constant the instruction states. It reaches the array theory as the initial
   contents of that range, which is what a store chain was standing in for. */
static ql_status encode_memory_image(product_encoder *encoder,
                                     product_side *side,
                                     const ql_ir_instruction_view_v1 *view) {
  size_t byte;
  ql_status status;

  if (view->image_size == 0u) {
    ql_error_set(encoder->error, QL_STATUS_TYPE_MISMATCH,
                 "a memory image states no bytes");
    return QL_STATUS_TYPE_MISMATCH;
  }
  status = term_add(encoder, "(and true");
  for (byte = 0u; byte < view->image_size && status == QL_STATUS_OK; ++byte) {
    status = term_add(encoder, " (= ");
    if (status == QL_STATUS_OK) {
      status = term_selected_byte(encoder, side, view, byte);
    }
    if (status == QL_STATUS_OK) {
      status = term_addf(encoder, " (_ bv%u %u))",
                         (unsigned)((const uint8_t *)view->image)[byte],
                         QL_PRODUCT_BYTE_WIDTH);
    }
  }
  if (status == QL_STATUS_OK) {
    status = term_add(encoder, ")");
  }
  return status;
}

/* An external call's results are free constants: nothing in this encoding
   says what the callee computes. What ties the two sides together is stated
   afterwards, as congruence between call sites. */
static ql_status declare_call_results(product_encoder *encoder,
                                      product_side *side,
                                      const ql_ir_instruction_view_v1 *view) {
  product_call *call;
  size_t index;
  ql_status status;

  if (side->call_count == QL_PRODUCT_MAX_CALL_SITES) {
    ql_error_set(encoder->error, QL_STATUS_TYPE_MISMATCH,
                 "a side makes more than %u calls, and congruence over them is "
                 "stated pairwise",
                 (unsigned)QL_PRODUCT_MAX_CALL_SITES);
    return QL_STATUS_TYPE_MISMATCH;
  }
  call = &side->calls[side->call_count];
  memset(call, 0, sizeof(*call));
  call->symbol = view->symbol;
  call->symbol_size = view->symbol_size;
  call->prefix = side->prefix;
  call->block = view->block;
  call->operand_count = view->operand_count;
  for (index = 0u; index < view->operand_count; ++index) {
    call->operands[index] = view->operands[index];
  }
  call->result_count = view->result_count;
  for (index = 0u; index < view->result_count; ++index) {
    const ql_ir_value_id result = view->results[index];
    const char *symbol = side_value_symbol(side, result);
    call->results[index] = result;
    switch (side->value_kinds[result]) {
    case QL_IR_TYPE_MEMORY:
      status = ql_smt2_builder_declare_array(
          encoder->builder, symbol, QL_PRODUCT_ADDRESS_WIDTH,
          QL_PRODUCT_BYTE_WIDTH, encoder->error);
      break;
    case QL_IR_TYPE_BOOL:
      status = ql_smt2_builder_declare_bool(encoder->builder, symbol,
                                            encoder->error);
      break;
    default:
      status = ql_smt2_builder_declare_bv(
          encoder->builder, symbol, side->value_widths[result], encoder->error);
      break;
    }
    if (status != QL_STATUS_OK) {
      return status;
    }
  }
  ++side->call_count;
  return QL_STATUS_OK;
}

static ql_status encode_load(product_encoder *encoder, product_side *side,
                             const ql_ir_instruction_view_v1 *view,
                             uint32_t width) {
  size_t byte_count;
  size_t byte;
  ql_status status = check_access_width(encoder, width, &byte_count);

  if (status != QL_STATUS_OK) {
    return status;
  }
  for (byte = 0u; byte + 1u < byte_count && status == QL_STATUS_OK; ++byte) {
    status = term_add(encoder, "(concat ");
  }
  if (status == QL_STATUS_OK) {
    status = term_selected_byte(encoder, side, view, byte_count - 1u);
  }
  for (byte = byte_count - 1u; byte != 0u && status == QL_STATUS_OK; --byte) {
    status = term_add(encoder, " ");
    if (status == QL_STATUS_OK) {
      status = term_selected_byte(encoder, side, view, byte - 1u);
    }
    if (status == QL_STATUS_OK) {
      status = term_add(encoder, ")");
    }
  }
  return status;
}

static ql_status encode_store(product_encoder *encoder, product_side *side,
                              const ql_ir_instruction_view_v1 *view,
                              uint32_t width) {
  size_t byte_count;
  size_t byte;
  ql_status status = check_access_width(encoder, width, &byte_count);

  if (status != QL_STATUS_OK) {
    return status;
  }
  for (byte = 0u; byte < byte_count && status == QL_STATUS_OK; ++byte) {
    status = term_add(encoder, "(store ");
  }
  if (status == QL_STATUS_OK) {
    status = term_add(encoder, side_value_symbol(side, view->operands[0]));
  }
  for (byte = 0u; byte < byte_count && status == QL_STATUS_OK; ++byte) {
    status = term_add(encoder, " ");
    if (status == QL_STATUS_OK) {
      status = term_byte_address(encoder, side, view->operands[1], byte);
    }
    if (status == QL_STATUS_OK) {
      status =
          term_addf(encoder, " ((_ extract %zu %zu) ",
                    byte * QL_PRODUCT_BYTE_WIDTH + QL_PRODUCT_BYTE_WIDTH - 1u,
                    byte * QL_PRODUCT_BYTE_WIDTH);
    }
    if (status == QL_STATUS_OK) {
      status = term_add(encoder, side_value_symbol(side, view->operands[2]));
    }
    if (status == QL_STATUS_OK) {
      status = term_add(encoder, "))");
    }
  }
  return status;
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
    return site_list_add(&side->guards, allocator, block, view->operands[0], 0u,
                         encoder->error);
  }
  if (view->opcode == QL_IR_OPCODE_CALL) {
    return declare_call_results(encoder, side, view);
  }
  if (view->opcode == QL_IR_OPCODE_ASSUME) {
    /* The model's alias-or-disjoint, first-page, and no-wrap constraints
       reach the query through this list. They are not restated here; the
       lowering already put them in the IR. */
    return site_list_add(&side->assumes, allocator, block, view->operands[0],
                         0u, encoder->error);
  }
  result = view->results[0];
  kind = side->value_kinds[result];
  width = side->value_widths[result];
  buffer_reset(&encoder->term);

  switch (view->opcode) {
  case QL_IR_OPCODE_IDENTITY:
    status = term_add(encoder, side_value_symbol(side, view->operands[0]));
    break;
  case QL_IR_OPCODE_BOOL_NOT:
    status = term_add(encoder, "(not ");
    if (status == QL_STATUS_OK) {
      status = term_add(encoder, side_value_symbol(side, view->operands[0]));
    }
    if (status == QL_STATUS_OK) {
      status = term_add(encoder, ")");
    }
    break;
  case QL_IR_OPCODE_BV_NOT:
  case QL_IR_OPCODE_BV_NEG:
    status = term_add(encoder, view->opcode == QL_IR_OPCODE_BV_NOT ? "(bvnot "
                                                                   : "(bvneg ");
    if (status == QL_STATUS_OK) {
      status = term_add(encoder, side_value_symbol(side, view->operands[0]));
    }
    if (status == QL_STATUS_OK) {
      status = term_add(encoder, ")");
    }
    break;
  case QL_IR_OPCODE_EQ:
  case QL_IR_OPCODE_NE:
    status =
        term_add(encoder, view->opcode == QL_IR_OPCODE_NE ? "(not (= " : "(= ");
    if (status == QL_STATUS_OK) {
      status = term_add(encoder, side_value_symbol(side, view->operands[0]));
    }
    if (status == QL_STATUS_OK) {
      status = term_add(encoder, " ");
    }
    if (status == QL_STATUS_OK) {
      status = term_add(encoder, side_value_symbol(side, view->operands[1]));
    }
    if (status == QL_STATUS_OK) {
      status = term_add(encoder, view->opcode == QL_IR_OPCODE_NE ? "))" : ")");
    }
    break;
  case QL_IR_OPCODE_SELECT:
    status = term_add(encoder, "(ite ");
    for (index = 0u; index < 3u && status == QL_STATUS_OK; ++index) {
      status =
          term_add(encoder, side_value_symbol(side, view->operands[index]));
      if (status == QL_STATUS_OK) {
        status = term_add(encoder, index == 2u ? ")" : " ");
      }
    }
    break;
  case QL_IR_OPCODE_ZEXT:
  case QL_IR_OPCODE_SEXT:
    status = term_addf(encoder, "((_ %s %u) ",
                       view->opcode == QL_IR_OPCODE_ZEXT ? "zero_extend"
                                                         : "sign_extend",
                       width - side->value_widths[view->operands[0]]);
    if (status == QL_STATUS_OK) {
      status = term_add(encoder, side_value_symbol(side, view->operands[0]));
    }
    if (status == QL_STATUS_OK) {
      status = term_add(encoder, ")");
    }
    break;
  case QL_IR_OPCODE_TRUNC:
    status = term_addf(encoder, "((_ extract %u 0) ", width - 1u);
    if (status == QL_STATUS_OK) {
      status = term_add(encoder, side_value_symbol(side, view->operands[0]));
    }
    if (status == QL_STATUS_OK) {
      status = term_add(encoder, ")");
    }
    break;
  case QL_IR_OPCODE_PTR_TO_BV:
  case QL_IR_OPCODE_BV_TO_PTR:
    /* The type rule fixes both sides to the same width, so an address and
       the integer that spells it are the same term. */
    status = term_add(encoder, side_value_symbol(side, view->operands[0]));
    break;
  case QL_IR_OPCODE_PTR_ADD:
    if (side->value_widths[view->operands[1]] !=
        side->value_widths[view->operands[0]]) {
      ql_error_set(encoder->error, QL_STATUS_TYPE_MISMATCH,
                   "IR adds a %u-bit offset to a %u-bit pointer; widening it "
                   "here would invent a rule the interpreter does not share",
                   side->value_widths[view->operands[1]],
                   side->value_widths[view->operands[0]]);
      return QL_STATUS_TYPE_MISMATCH;
    }
    status = term_add(encoder, "(bvadd ");
    if (status == QL_STATUS_OK) {
      status = term_add(encoder, side_value_symbol(side, view->operands[0]));
    }
    if (status == QL_STATUS_OK) {
      status = term_add(encoder, " ");
    }
    if (status == QL_STATUS_OK) {
      status = term_add(encoder, side_value_symbol(side, view->operands[1]));
    }
    if (status == QL_STATUS_OK) {
      status = term_add(encoder, ")");
    }
    break;
  case QL_IR_OPCODE_MEMORY_IMAGE:
    status = encode_memory_image(encoder, side, view);
    break;
  case QL_IR_OPCODE_LOAD:
    status =
        encode_load(encoder, side, view, kind == QL_IR_TYPE_BOOL ? 1u : width);
    break;
  case QL_IR_OPCODE_STORE:
    status =
        encode_store(encoder, side, view,
                     side->value_kinds[view->operands[2]] == QL_IR_TYPE_BOOL
                         ? 1u
                         : side->value_widths[view->operands[2]]);
    break;
  case QL_IR_OPCODE_PHI:
    status = QL_STATUS_OK;
    for (index = 0u; index + 1u < view->operand_count && status == QL_STATUS_OK;
         ++index) {
      status = term_add(encoder, "(ite ");
      if (status == QL_STATUS_OK) {
        status =
            term_edge_symbol(encoder, side, view->block_operands[index], block);
      }
      if (status == QL_STATUS_OK) {
        status = term_add(encoder, " ");
      }
      if (status == QL_STATUS_OK) {
        status =
            term_add(encoder, side_value_symbol(side, view->operands[index]));
      }
      if (status == QL_STATUS_OK) {
        status = term_add(encoder, " ");
      }
    }
    if (status == QL_STATUS_OK) {
      status = term_add(
          encoder,
          side_value_symbol(side, view->operands[view->operand_count - 1u]));
    }
    for (index = 0u; index + 1u < view->operand_count && status == QL_STATUS_OK;
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
      status = term_add(encoder, side_value_symbol(side, view->operands[0]));
    }
    if (status == QL_STATUS_OK) {
      status = term_add(encoder, " ");
    }
    if (status == QL_STATUS_OK) {
      status = term_add(encoder, side_value_symbol(side, view->operands[1]));
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
    status =
        emit_sorted(encoder, side_value_symbol(side, (ql_ir_value_id)index),
                    side->value_kinds[index], side->value_widths[index]);
    if (status != QL_STATUS_OK) {
      return status;
    }
  }
  return QL_STATUS_OK;
}

static ql_status encode_block_reach(product_encoder *encoder,
                                    product_side *side, ql_ir_block_id block) {
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
        status =
            term_edge_symbol(encoder, side, side->edges[index].from, block);
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
        status =
            term_edge_symbol(encoder, side, side->edges[index].from, block);
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
                   (unsigned)block->id, (unsigned)block->terminator.target);
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
                   (unsigned)block->id, (unsigned)block->terminator.target);
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
    status =
        term_add(encoder, side_value_symbol(side, block->terminator.condition));
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
                 (unsigned)block->id, (unsigned)block->terminator.false_target);
  buffer_reset(&encoder->term);
  status = term_add(encoder, "(and ");
  if (status == QL_STATUS_OK) {
    status = term_block_symbol(encoder, side, block->id);
  }
  if (status == QL_STATUS_OK) {
    status = term_add(encoder, " (not ");
  }
  if (status == QL_STATUS_OK) {
    status =
        term_add(encoder, side_value_symbol(side, block->terminator.condition));
  }
  if (status == QL_STATUS_OK) {
    status = term_add(encoder, "))");
  }
  if (status == QL_STATUS_OK) {
    status = emit_bool(encoder, symbol);
  }
  return status;
}

static ql_status encode_reachability_disjunction(product_encoder *encoder,
                                                 product_side *side,
                                                 const product_site_list *list,
                                                 const char *symbol) {
  size_t index;
  ql_status status;

  buffer_reset(&encoder->term);
  if (list->count == 0u) {
    status = term_add(encoder, "false");
  } else if (list->count == 1u) {
    status = term_block_symbol(encoder, side, list->items[0].block);
  } else {
    status = term_add(encoder, "(or");
    for (index = 0u; index < list->count && status == QL_STATUS_OK; ++index) {
      status = term_add(encoder, " ");
      if (status == QL_STATUS_OK) {
        status = term_block_symbol(encoder, side, list->items[index].block);
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

static size_t side_memory_carrier_count(const product_side *side) {
  size_t index;
  size_t count = 0u;

  for (index = 0u; index < side->returns.count; ++index) {
    if (side->returns.items[index].memory != QL_IR_INVALID_VALUE_ID) {
      ++count;
    }
  }
  return count;
}

static const product_site *side_memory_carrier_at(const product_side *side,
                                                  size_t ordinal) {
  size_t index;

  for (index = 0u; index < side->returns.count; ++index) {
    if (side->returns.items[index].memory == QL_IR_INVALID_VALUE_ID) {
      continue;
    }
    if (ordinal == 0u) {
      return &side->returns.items[index];
    }
    --ordinal;
  }
  return NULL;
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
        status =
            term_block_symbol(encoder, side, side->guards.items[index].block);
      }
      if (status == QL_STATUS_OK) {
        status = term_add(encoder, " ");
      }
      if (status == QL_STATUS_OK) {
        status = term_add(
            encoder, side_value_symbol(side, side->guards.items[index].value));
      }
      if (status == QL_STATUS_OK) {
        status = term_add(encoder, ")");
      }
    }
    for (index = 0u; index < side->undefined.count && status == QL_STATUS_OK;
         ++index) {
      status = term_add(encoder, " (not ");
      if (status == QL_STATUS_OK) {
        status = term_block_symbol(encoder, side,
                                   side->undefined.items[index].block);
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
  status =
      encode_reachability_disjunction(encoder, side, &side->returns, symbol);
  if (status != QL_STATUS_OK) {
    return status;
  }
  (void)snprintf(symbol, sizeof(symbol), "%c_traps", side->prefix);
  status = encode_reachability_disjunction(encoder, side, &side->traps, symbol);
  if (status != QL_STATUS_OK) {
    return status;
  }
  (void)snprintf(symbol, sizeof(symbol), "%c_diverges", side->prefix);
  status =
      encode_reachability_disjunction(encoder, side, &side->diverges, symbol);
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
    (void)snprintf(symbol, sizeof(symbol), "%c_return_value", side->prefix);
    buffer_reset(&encoder->term);
    if (side->returns.count == 0u) {
      status = term_zero(encoder, side->return_kind, side->return_width);
    } else {
      status = QL_STATUS_OK;
      for (index = 0u;
           index + 1u < side->returns.count && status == QL_STATUS_OK;
           ++index) {
        status = term_add(encoder, "(ite ");
        if (status == QL_STATUS_OK) {
          status = term_block_symbol(encoder, side,
                                     side->returns.items[index].block);
        }
        if (status == QL_STATUS_OK) {
          status = term_add(encoder, " ");
        }
        if (status == QL_STATUS_OK) {
          status = term_add(
              encoder,
              side_value_symbol(side, side->returns.items[index].value));
        }
        if (status == QL_STATUS_OK) {
          status = term_add(encoder, " ");
        }
      }
      if (status == QL_STATUS_OK) {
        status = term_add(
            encoder,
            side_value_symbol(
                side, side->returns.items[side->returns.count - 1u].value));
      }
      for (index = 0u;
           index + 1u < side->returns.count && status == QL_STATUS_OK;
           ++index) {
        status = term_add(encoder, ")");
      }
    }
    if (status == QL_STATUS_OK) {
      status =
          emit_sorted(encoder, symbol, side->return_kind, side->return_width);
    }
    if (status != QL_STATUS_OK) {
      return status;
    }
  }

  /* The history a run leaves behind is the history of whichever return it
     reached, on the same footing as its memory. A run that traps or
     diverges leaves none, and the comparison is gated on termination for
     exactly that reason. */
  if (side->call_count != 0u) {
    size_t carrier;
    size_t carriers = 0u;

    for (index = 0u; index < side->returns.count; ++index) {
      if (side->returns.items[index].trace != QL_IR_INVALID_VALUE_ID) {
        ++carriers;
      }
    }
    (void)snprintf(symbol, sizeof(symbol), "%c_final_trace", side->prefix);
    buffer_reset(&encoder->term);
    status = QL_STATUS_OK;
    if (carriers == 0u) {
      status = term_add(encoder, QL_PRODUCT_TRACE_SYMBOL);
    } else {
      size_t seen = 0u;
      for (index = 0u; index < side->returns.count && status == QL_STATUS_OK;
           ++index) {
        if (side->returns.items[index].trace == QL_IR_INVALID_VALUE_ID) {
          continue;
        }
        ++seen;
        if (seen == carriers) {
          status = term_add(
              encoder,
              side_value_symbol(side, side->returns.items[index].trace));
          break;
        }
        status = term_add(encoder, "(ite ");
        if (status == QL_STATUS_OK) {
          status = term_block_symbol(encoder, side,
                                     side->returns.items[index].block);
        }
        if (status == QL_STATUS_OK) {
          status = term_add(encoder, " ");
        }
        if (status == QL_STATUS_OK) {
          status = term_add(
              encoder,
              side_value_symbol(side, side->returns.items[index].trace));
        }
        if (status == QL_STATUS_OK) {
          status = term_add(encoder, " ");
        }
      }
      for (carrier = 0u; carrier + 1u < carriers && status == QL_STATUS_OK;
           ++carrier) {
        status = term_add(encoder, ")");
      }
    }
    if (status == QL_STATUS_OK) {
      status = emit_bv(encoder, symbol, QL_PRODUCT_TRACE_WIDTH);
    }
    if (status != QL_STATUS_OK) {
      return status;
    }
  }

  /* The memory a run leaves behind is the memory of whichever return it
     reached. A run that traps or diverges leaves none, and the comparison
     below is gated on both sides terminating for exactly that reason. */
  if (side->object_count != 0u) {
    const size_t carriers = side_memory_carrier_count(side);
    size_t carrier;

    (void)snprintf(symbol, sizeof(symbol), "%c_final_memory", side->prefix);
    buffer_reset(&encoder->term);
    status = QL_STATUS_OK;
    if (carriers == 0u) {
      /* Nothing returns, so nothing is left behind. The initial memory
         stands and the gate below keeps it from being observed. */
      status = term_add(encoder, QL_PRODUCT_MEMORY_SYMBOL);
    } else {
      for (carrier = 0u; carrier + 1u < carriers && status == QL_STATUS_OK;
           ++carrier) {
        const product_site *site = side_memory_carrier_at(side, carrier);
        status = term_add(encoder, "(ite ");
        if (status == QL_STATUS_OK) {
          status = term_block_symbol(encoder, side, site->block);
        }
        if (status == QL_STATUS_OK) {
          status = term_add(encoder, " ");
        }
        if (status == QL_STATUS_OK) {
          status = term_add(encoder, side_value_symbol(side, site->memory));
        }
        if (status == QL_STATUS_OK) {
          status = term_add(encoder, " ");
        }
      }
      if (status == QL_STATUS_OK) {
        status = term_add(
            encoder,
            side_value_symbol(
                side, side_memory_carrier_at(side, carriers - 1u)->memory));
      }
      for (carrier = 0u; carrier + 1u < carriers && status == QL_STATUS_OK;
           ++carrier) {
        status = term_add(encoder, ")");
      }
    }
    if (status == QL_STATUS_OK) {
      status = emit_memory(encoder, symbol);
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
      status = term_block_symbol(encoder, side, side->traps.items[index].block);
    }
    if (status == QL_STATUS_OK) {
      status = term_addf(encoder, " (_ bv%llu 64) ",
                         (unsigned long long)side->traps.items[index].code);
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
  for (order_index = 0u; order_index < side->view.block_count; ++order_index) {
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
      status =
          encode_instruction(encoder, side, &instruction, block_id, allocator);
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
                             block.terminator.return_value, 0u, encoder->error);
      if (status == QL_STATUS_OK) {
        side->returns.items[side->returns.count - 1u].memory =
            block.terminator.memory;
        side->returns.items[side->returns.count - 1u].trace =
            block.terminator.event_trace;
      }
      break;
    case QL_IR_TERMINATOR_TRAP:
      status = site_list_add(&side->traps, allocator, block_id,
                             QL_IR_INVALID_VALUE_ID, block.terminator.code,
                             encoder->error);
      break;
    case QL_IR_TERMINATOR_UNDEFINED_BEHAVIOR:
      status = site_list_add(&side->undefined, allocator, block_id,
                             QL_IR_INVALID_VALUE_ID, 0u, encoder->error);
      break;
    case QL_IR_TERMINATOR_DIVERGE:
      status = site_list_add(&side->diverges, allocator, block_id,
                             QL_IR_INVALID_VALUE_ID, 0u, encoder->error);
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
  status =
      ql_precondition_node_at(precondition, node_index, &node, encoder->error);
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
    return term_add(encoder, encoder->inputs[node.argument_index].symbol);
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
      status =
          term_add_bytes(encoder, node.integer_value, node.integer_value_size);
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
                 "precondition node kind %u needs a memory model that the "
                 "scalar miter does not have",
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
      status =
          encode_precondition_node(encoder, precondition, node.children[index]);
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
      signature, &signature_view, storage, QL_SOURCE_SIGNATURE_MAX_ARGUMENTS,
      encoder->error);
  if (status != QL_STATUS_OK) {
    return status;
  }
  status =
      ql_precondition_parse(encoder->allocator, contract->precondition_json,
                            contract->precondition_json_size, &signature_view,
                            &precondition, encoder->error);
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

/* Every ASSUME the lowering emitted, conditioned on its block being reached.
   These carry the memory model's standing constraints into the query, so both
   the violation and the domain range over admissible object layouts only. */
/* Two call sites can be the same call. They are when they name the same
   callee and are handed the same things: the same history, the same memory,
   and the same arguments. That is all "the callee is a function" says, and it
   is the whole of what makes a pair of calls cancel.

   Nothing here says what any callee computes. A pair that does not match on
   every operand gets no equality at all, which is why a different callee or a
   different argument leaves the two sides free to disagree and the miter
   returns no verdict rather than a wrong one. */
static ql_status term_call_congruence(product_encoder *encoder,
                                      const product_side *left_side,
                                      const product_call *left,
                                      const product_side *right_side,
                                      const product_call *right) {
  size_t index;
  ql_status status;

  if (left->symbol_size != right->symbol_size ||
      memcmp(left->symbol, right->symbol, left->symbol_size) != 0 ||
      left->operand_count != right->operand_count ||
      left->result_count != right->result_count) {
    return QL_STATUS_OK;
  }
  status = term_add(encoder, " (=> (and true");
  for (index = 0u; index < left->operand_count && status == QL_STATUS_OK;
       ++index) {
    status = term_add(encoder, " (= ");
    if (status == QL_STATUS_OK) {
      status = term_add(encoder, side_value_symbol((product_side *)left_side,
                                                   left->operands[index]));
    }
    if (status == QL_STATUS_OK) {
      status = term_add(encoder, " ");
    }
    if (status == QL_STATUS_OK) {
      status = term_add(encoder, side_value_symbol((product_side *)right_side,
                                                   right->operands[index]));
    }
    if (status == QL_STATUS_OK) {
      status = term_add(encoder, ")");
    }
  }
  if (status == QL_STATUS_OK) {
    status = term_add(encoder, ") (and true");
  }
  for (index = 0u;
       index < left->result_count && status == QL_STATUS_OK; ++index) {
    status = term_add(encoder, " (= ");
    if (status == QL_STATUS_OK) {
      status = term_add(encoder, side_value_symbol((product_side *)left_side,
                                                   left->results[index]));
    }
    if (status == QL_STATUS_OK) {
      status = term_add(encoder, " ");
    }
    if (status == QL_STATUS_OK) {
      status = term_add(encoder, side_value_symbol((product_side *)right_side,
                                                   right->results[index]));
    }
    if (status == QL_STATUS_OK) {
      status = term_add(encoder, ")");
    }
  }
  if (status == QL_STATUS_OK) {
    status = term_add(encoder, "))");
  }
  return status;
}

/* Congruence over every ordered pair of call sites on both sides. The pairs
   within one side matter too: a body that calls the same function twice with
   the same arguments has to get the same answer both times, or the two sides
   could not be compared through it. */
/* How many call pairs congruence has anything to say about. A conjunction
   with nothing in it is not a smaller conjunction, it is `true`. */
static size_t count_call_congruences(const product_side *left,
                                     const product_side *right) {
  const product_side *sides[2];
  size_t outer_side;
  size_t inner_side;
  size_t outer;
  size_t inner;
  size_t count = 0u;

  sides[0] = left;
  sides[1] = right;
  for (outer_side = 0u; outer_side < 2u; ++outer_side) {
    for (outer = 0u; outer < sides[outer_side]->call_count; ++outer) {
      const product_call *a = &sides[outer_side]->calls[outer];
      for (inner_side = outer_side; inner_side < 2u; ++inner_side) {
        const size_t first = inner_side == outer_side ? outer + 1u : 0u;
        for (inner = first; inner < sides[inner_side]->call_count; ++inner) {
          const product_call *b = &sides[inner_side]->calls[inner];
          if (a->symbol_size == b->symbol_size &&
              memcmp(a->symbol, b->symbol, a->symbol_size) == 0 &&
              a->operand_count == b->operand_count &&
              a->result_count == b->result_count) {
            ++count;
          }
        }
      }
    }
  }
  return count;
}

static ql_status term_all_call_congruences(product_encoder *encoder,
                                           const product_side *left,
                                           const product_side *right) {
  const product_side *sides[2];
  size_t outer_side;
  size_t inner_side;
  size_t outer;
  size_t inner;
  ql_status status = QL_STATUS_OK;

  sides[0] = left;
  sides[1] = right;
  for (outer_side = 0u; outer_side < 2u && status == QL_STATUS_OK;
       ++outer_side) {
    for (outer = 0u;
         outer < sides[outer_side]->call_count && status == QL_STATUS_OK;
         ++outer) {
      for (inner_side = outer_side; inner_side < 2u && status == QL_STATUS_OK;
           ++inner_side) {
        const size_t first = inner_side == outer_side ? outer + 1u : 0u;
        for (inner = first;
             inner < sides[inner_side]->call_count && status == QL_STATUS_OK;
             ++inner) {
          status = term_call_congruence(
              encoder, sides[outer_side], &sides[outer_side]->calls[outer],
              sides[inner_side], &sides[inner_side]->calls[inner]);
        }
      }
    }
  }
  return status;
}

static ql_status encode_assumptions(product_encoder *encoder,
                                    const product_side *left,
                                    const product_side *right) {
  const product_side *sides[2];
  size_t which;
  size_t index;
  ql_status status;

  sides[0] = left;
  sides[1] = right;
  buffer_reset(&encoder->term);
  if (left->assumes.count == 0u && right->assumes.count == 0u &&
      count_call_congruences(left, right) == 0u) {
    status = term_add(encoder, "true");
    return status == QL_STATUS_OK
               ? emit_bool(encoder, QL_PRODUCT_ASSUMPTION_SYMBOL)
               : status;
  }
  status = term_add(encoder, "(and true");
  if (status == QL_STATUS_OK) {
    /* Congruence is an assumption about the environment, not a claim
       about either program, so it sits with the model's other standing
       constraints and the domain query gets to check that it leaves
       something to compare. */
    status = term_all_call_congruences(encoder, left, right);
  }
  for (which = 0u; which < 2u && status == QL_STATUS_OK; ++which) {
    const product_side *side = sides[which];
    for (index = 0u; index < side->assumes.count && status == QL_STATUS_OK;
         ++index) {
      status = term_add(encoder, " (=> ");
      if (status == QL_STATUS_OK) {
        status =
            term_block_symbol(encoder, side, side->assumes.items[index].block);
      }
      if (status == QL_STATUS_OK) {
        status = term_add(encoder, " ");
      }
      if (status == QL_STATUS_OK) {
        status = term_add(encoder,
                          side_value_symbol((product_side *)side,
                                            side->assumes.items[index].value));
      }
      if (status == QL_STATUS_OK) {
        status = term_add(encoder, ")");
      }
    }
  }
  if (status == QL_STATUS_OK) {
    status = term_add(encoder, ")");
  }
  if (status != QL_STATUS_OK) {
    return status;
  }
  return emit_bool(encoder, QL_PRODUCT_ASSUMPTION_SYMBOL);
}

/* The probe lies in some object. Everything outside the object table is not
   externally reachable storage, so nothing is claimed about it. */
static ql_status term_probe_in_range(product_encoder *encoder) {
  size_t index;
  ql_status status = term_add(encoder, "(or false");

  for (index = 0u; index < encoder->object_count && status == QL_STATUS_OK;
       ++index) {
    status =
        term_addf(encoder, " (and (bvule %s %s) (bvult %s (bvadd %s %s)))",
                  encoder->objects[index].base_symbol, QL_PRODUCT_PROBE_SYMBOL,
                  QL_PRODUCT_PROBE_SYMBOL, encoder->objects[index].base_symbol,
                  encoder->objects[index].size_symbol);
  }
  if (status == QL_STATUS_OK) {
    status = term_add(encoder, ")");
  }
  return status;
}

/* Comparing the return-value projection means comparing whether a normal
   return happened at all, then the value. A trapping or diverging execution
   produces no return value, which is a different observation from any value. */
static ql_status
encode_observation_equality(product_encoder *encoder,
                            const ql_semantic_contract_v1 *contract,
                            int compare_return_value, int compare_traces) {
  ql_status status;

  buffer_reset(&encoder->term);
  status = term_add(encoder, "(and true");
  if (status == QL_STATUS_OK &&
      (contract->observations & QL_OBSERVE_MEMORY) != 0u &&
      encoder->object_count != 0u) {
    /* Both sides run over the same objects, so the final states agree
       exactly when they agree byte by byte inside those objects. A
       diverging or trapping run leaves no final memory, which is why the
       claim is gated on both sides terminating. */
    status = term_add(encoder, " (=> (and l_terminates r_terminates ");
    if (status == QL_STATUS_OK) {
      status = term_probe_in_range(encoder);
    }
    if (status == QL_STATUS_OK) {
      status = term_addf(
          encoder,
          ") (= (select l_final_memory %s) (select r_final_memory %s)))",
          QL_PRODUCT_PROBE_SYMBOL, QL_PRODUCT_PROBE_SYMBOL);
    }
  }
  if (status == QL_STATUS_OK &&
      (contract->observations & QL_OBSERVE_RETURN_VALUE) != 0u) {
    status = term_add(encoder, " (= l_returns r_returns)");
    if (status == QL_STATUS_OK && compare_return_value) {
      status = term_add(encoder,
                        " (=> l_returns (= l_return_value r_return_value))");
    }
  }
  if (status == QL_STATUS_OK &&
      (contract->observations & QL_OBSERVE_TERMINATION) != 0u) {
    status = term_add(encoder, " (= l_terminates r_terminates)");
  }
  if (status == QL_STATUS_OK && compare_traces) {
    /* The order the calls happened in is what the history records, so a
       contract that observes external calls in order compares the
       histories the two runs ended with. Congruence is what lets two
       identical call sequences reach the same one. */
    status = term_add(encoder, " (=> (and l_terminates r_terminates)"
                               " (= l_final_trace r_final_trace))");
  }
  if (status == QL_STATUS_OK &&
      (contract->observations & QL_OBSERVE_TRAPS) != 0u) {
    status = term_add(encoder, " (= l_traps r_traps)");
    if (status == QL_STATUS_OK) {
      status = term_add(encoder, " (=> l_traps (= l_trap_code r_trap_code))");
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
  status = term_add(encoder, "(and " QL_PRODUCT_PRECONDITION_SYMBOL
                             " " QL_PRODUCT_ASSUMPTION_SYMBOL " ");
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
        " (and l_defined r_defined (not " QL_PRODUCT_OBSERVATION_SYMBOL ")))";
    break;
  case QL_UB_LANGUAGE_REFINEMENT:
    switch (contract->relation) {
    case QL_RELATION_LEFT_REFINES_RIGHT:
      body = "(and r_defined (or (not l_defined) "
             "(not " QL_PRODUCT_OBSERVATION_SYMBOL ")))";
      break;
    case QL_RELATION_RIGHT_REFINES_LEFT:
      body = "(and l_defined (or (not r_defined) "
             "(not " QL_PRODUCT_OBSERVATION_SYMBOL ")))";
      break;
    default:
      body =
          "(or (not (= l_defined r_defined))"
          " (and l_defined r_defined (not " QL_PRODUCT_OBSERVATION_SYMBOL ")))";
      break;
    }
    break;
  default:
    body = "(and l_defined r_defined (not " QL_PRODUCT_OBSERVATION_SYMBOL "))";
    break;
  }
  buffer_reset(&encoder->term);
  status = term_add(encoder, "(and " QL_PRODUCT_PRECONDITION_SYMBOL
                             " " QL_PRODUCT_ASSUMPTION_SYMBOL " ");
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
      query->allocator.user_data, argument_count * QL_PRODUCT_SYMBOL_CAPACITY);
  if (query->inputs == NULL || query->input_symbols == NULL) {
    ql_error_set(error, QL_STATUS_OUT_OF_MEMORY, NULL);
    return QL_STATUS_OUT_OF_MEMORY;
  }
  memset(query->inputs, 0, argument_count * sizeof(*query->inputs));
  for (index = 0u; index < argument_count; ++index) {
    char *symbol = query->input_symbols + index * QL_PRODUCT_SYMBOL_CAPACITY;
    ql_problem_argument_binding_v1 binding;
    ql_source_type_v1 type;
    size_t search;
    int written;

    memset(&binding, 0, sizeof(binding));
    binding.left_index = UINT32_MAX;
    for (search = 0u; search < argument_count; ++search) {
      ql_problem_argument_binding_v1 candidate;
      status =
          ql_problem_argument_binding_at(problem, search, &candidate, error);
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
                   "argument correspondence is missing left index %zu", index);
      return QL_STATUS_INTERNAL_ERROR;
    }
    status =
        ql_source_signature_argument_at(left_signature, index, &type, error);
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

/* Counts the objects a side declares and records where each one's base
   parameter sits, by reading the parameter list rather than by predicting it.
   Predicting worked while every object was a pointer argument's; it stops
   working the moment a lowering makes one of its own -- for a global, a local
   array or record, a string literal -- or threads an event trace. */
static ql_status collect_object_parameters(const ql_ir *ir,
                                           const ql_allocator *allocator,
                                           uint32_t **indices, size_t *count,
                                           ql_error *error) {
  ql_ir_view_v1 view;
  size_t index;
  size_t found = 0u;
  size_t capacity = 0u;
  uint32_t *list = NULL;
  ql_status status;

  *indices = NULL;
  *count = 0u;
  memset(&view, 0, sizeof(view));
  view.struct_size = sizeof(view);
  status = ql_ir_get_view(ir, &view, error);
  if (status != QL_STATUS_OK) {
    return status;
  }
  for (index = 0u; index < view.value_count; ++index) {
    ql_ir_value_view_v1 value;
    memset(&value, 0, sizeof(value));
    value.struct_size = sizeof(value);
    status = ql_ir_value_at(ir, index, &value, error);
    if (status != QL_STATUS_OK) {
      allocator->deallocate(allocator->user_data, list);
      return status;
    }
    if (value.definition_kind != QL_IR_VALUE_PARAMETER || value.name == NULL ||
        strstr(value.name, ".__base") == NULL) {
      continue;
    }
    if (found == capacity) {
      const size_t next = capacity == 0u ? 8u : capacity * 2u;
      void *grown = allocator->reallocate(allocator->user_data, list,
                                          next * sizeof(*list));
      if (grown == NULL) {
        allocator->deallocate(allocator->user_data, list);
        ql_error_set(error, QL_STATUS_OUT_OF_MEMORY, NULL);
        return QL_STATUS_OUT_OF_MEMORY;
      }
      list = (uint32_t *)grown;
      capacity = next;
    }
    list[found++] = value.id;
  }
  *indices = list;
  *count = found;
  return QL_STATUS_OK;
}

/* The object table is the objects each side declares, in the order it
   declares them. The pointer arguments' objects come first and keep the
   argument correspondence, so a problem that permutes the two argument lists
   still binds the same storage to the same symbol; the objects a lowering
   made for itself follow, and correspond by position, because nothing in the
   source signature names them. Both sides must declare the same number, or
   one is reasoning about storage the other does not have. */
static ql_status build_objects(ql_product_query *query, const ql_ir *left_ir,
                               const ql_ir *right_ir, uint32_t **left_map,
                               uint32_t **right_map, ql_error *error) {
  uint32_t *left_bases = NULL;
  uint32_t *right_bases = NULL;
  size_t left_count = 0u;
  size_t right_count = 0u;
  size_t pointer_count = 0u;
  size_t index;
  size_t next = 0u;
  ql_status status = collect_object_parameters(left_ir, &query->allocator,
                                               &left_bases, &left_count, error);

  if (status != QL_STATUS_OK) {
    return status;
  }
  status = collect_object_parameters(right_ir, &query->allocator, &right_bases,
                                     &right_count, error);
  if (status != QL_STATUS_OK) {
    query->allocator.deallocate(query->allocator.user_data, left_bases);
    return status;
  }
  if (left_count != right_count) {
    query->allocator.deallocate(query->allocator.user_data, left_bases);
    query->allocator.deallocate(query->allocator.user_data, right_bases);
    ql_error_set(error, QL_STATUS_TYPE_MISMATCH,
                 "the two sides declare %zu and %zu objects, so they do not "
                 "describe the same storage",
                 left_count, right_count);
    return QL_STATUS_TYPE_MISMATCH;
  }
  for (index = 0u; index < query->view.input_count; ++index) {
    if (query->inputs[index].kind == QL_SOURCE_TYPE_POINTER) {
      ++pointer_count;
    }
  }
  if (left_count < pointer_count) {
    query->allocator.deallocate(query->allocator.user_data, left_bases);
    query->allocator.deallocate(query->allocator.user_data, right_bases);
    ql_error_set(error, QL_STATUS_TYPE_MISMATCH,
                 "the sides declare %zu objects for %zu pointer arguments",
                 left_count, pointer_count);
    return QL_STATUS_TYPE_MISMATCH;
  }
  query->object_count = left_count;
  if (left_count == 0u) {
    query->allocator.deallocate(query->allocator.user_data, left_bases);
    query->allocator.deallocate(query->allocator.user_data, right_bases);
    return QL_STATUS_OK;
  }
  query->objects = query->allocator.allocate(
      query->allocator.user_data, left_count * sizeof(*query->objects));
  *left_map = query->allocator.allocate(query->allocator.user_data,
                                        left_count * sizeof(**left_map));
  *right_map = query->allocator.allocate(query->allocator.user_data,
                                         left_count * sizeof(**right_map));
  if (query->objects == NULL || *left_map == NULL || *right_map == NULL) {
    query->allocator.deallocate(query->allocator.user_data, left_bases);
    query->allocator.deallocate(query->allocator.user_data, right_bases);
    ql_error_set(error, QL_STATUS_OUT_OF_MEMORY, NULL);
    return QL_STATUS_OUT_OF_MEMORY;
  }
  memset(query->objects, 0, left_count * sizeof(*query->objects));

  /* The pointer arguments' objects, in left argument order. */
  for (index = 0u; index < query->view.input_count; ++index) {
    const ql_product_input_v1 *input = &query->inputs[index];
    product_object *object;
    size_t right_rank = 0u;
    size_t scan;

    if (input->kind != QL_SOURCE_TYPE_POINTER) {
      continue;
    }
    for (scan = 0u; scan < query->view.input_count; ++scan) {
      if (query->inputs[scan].kind == QL_SOURCE_TYPE_POINTER &&
          query->inputs[scan].right_parameter < input->right_parameter) {
        ++right_rank;
      }
    }
    object = &query->objects[next];
    if (snprintf(object->base_symbol, sizeof(object->base_symbol),
                 "obj%zu_base", next) < 0 ||
        snprintf(object->size_symbol, sizeof(object->size_symbol),
                 "obj%zu_size", next) < 0) {
      query->allocator.deallocate(query->allocator.user_data, left_bases);
      query->allocator.deallocate(query->allocator.user_data, right_bases);
      ql_error_set(error, QL_STATUS_INTERNAL_ERROR,
                   "could not format a product object symbol");
      return QL_STATUS_INTERNAL_ERROR;
    }
    object->left_base_parameter = left_bases[next];
    object->right_base_parameter = right_bases[right_rank];
    (*left_map)[next] = (uint32_t)next;
    (*right_map)[right_rank] = (uint32_t)next;
    ++next;
  }
  /* Then the objects the lowerings made for themselves, by position. */
  for (; next < left_count; ++next) {
    product_object *object = &query->objects[next];
    if (snprintf(object->base_symbol, sizeof(object->base_symbol),
                 "obj%zu_base", next) < 0 ||
        snprintf(object->size_symbol, sizeof(object->size_symbol),
                 "obj%zu_size", next) < 0) {
      query->allocator.deallocate(query->allocator.user_data, left_bases);
      query->allocator.deallocate(query->allocator.user_data, right_bases);
      ql_error_set(error, QL_STATUS_INTERNAL_ERROR,
                   "could not format a product object symbol");
      return QL_STATUS_INTERNAL_ERROR;
    }
    object->left_base_parameter = left_bases[next];
    object->right_base_parameter = right_bases[next];
    (*left_map)[next] = (uint32_t)next;
    (*right_map)[next] = (uint32_t)next;
  }
  query->allocator.deallocate(query->allocator.user_data, left_bases);
  query->allocator.deallocate(query->allocator.user_data, right_bases);
  return QL_STATUS_OK;
}

static ql_status
declare_inputs(ql_smt2_builder *builder, const ql_product_input_v1 *inputs,
               size_t input_count, const product_object *objects,
               size_t object_count, uint32_t *maximum_width, ql_error *error) {
  size_t index;
  ql_status status;

  *maximum_width = 1u;
  for (index = 0u; index < input_count; ++index) {
    if (inputs[index].kind == QL_SOURCE_TYPE_BOOL) {
      status =
          ql_smt2_builder_declare_bool(builder, inputs[index].symbol, error);
    } else if (inputs[index].kind == QL_SOURCE_TYPE_SIGNED_INTEGER ||
               inputs[index].kind == QL_SOURCE_TYPE_UNSIGNED_INTEGER ||
               inputs[index].kind == QL_SOURCE_TYPE_POINTER) {
      if (inputs[index].bit_width > *maximum_width) {
        *maximum_width = inputs[index].bit_width;
      }
      status = ql_smt2_builder_declare_bv(builder, inputs[index].symbol,
                                          inputs[index].bit_width, error);
    } else {
      ql_error_set(error, QL_STATUS_TYPE_MISMATCH,
                   "argument %zu has a source type this miter does not model",
                   index);
      return QL_STATUS_TYPE_MISMATCH;
    }
    if (status != QL_STATUS_OK) {
      return status;
    }
  }
  if (object_count == 0u) {
    return QL_STATUS_OK;
  }
  if (*maximum_width < QL_PRODUCT_ADDRESS_WIDTH) {
    *maximum_width = QL_PRODUCT_ADDRESS_WIDTH;
  }
  status = ql_smt2_builder_declare_array(builder, QL_PRODUCT_MEMORY_SYMBOL,
                                         QL_PRODUCT_ADDRESS_WIDTH,
                                         QL_PRODUCT_BYTE_WIDTH, error);
  if (status == QL_STATUS_OK) {
    status = ql_smt2_builder_declare_bv(builder, QL_PRODUCT_PROBE_SYMBOL,
                                        QL_PRODUCT_ADDRESS_WIDTH, error);
  }
  for (index = 0u; index < object_count && status == QL_STATUS_OK; ++index) {
    status = ql_smt2_builder_declare_bv(builder, objects[index].base_symbol,
                                        QL_PRODUCT_ADDRESS_WIDTH, error);
    if (status == QL_STATUS_OK) {
      status = ql_smt2_builder_declare_bv(builder, objects[index].size_symbol,
                                          QL_PRODUCT_ADDRESS_WIDTH, error);
    }
  }
  return status;
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
  ql_artifact_release(query->bounded_violation);
  ql_artifact_release(query->domain);
  allocator.deallocate(allocator.user_data, query->inputs);
  allocator.deallocate(allocator.user_data, query->input_symbols);
  allocator.deallocate(allocator.user_data, query->objects);
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
  uint32_t *left_object_map = NULL;
  uint32_t *right_object_map = NULL;
  int left_threads_a_trace = 0;
  int right_threads_a_trace = 0;
  uint32_t maximum_width = 1u;
  ql_solver_logic logic = QL_SOLVER_LOGIC_QF_BV;
  size_t index;

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

  status = ql_source_signature_open(selected,
                                    ql_problem_left_signature_artifact(problem),
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
    status = ql_source_signature_bind_ir(right_signature, right_ir, error);
  }
  if (status != QL_STATUS_OK) {
    goto cleanup;
  }
  memset(&left_signature_view, 0, sizeof(left_signature_view));
  left_signature_view.struct_size = sizeof(left_signature_view);
  status =
      ql_source_signature_get_view(left_signature, &left_signature_view, error);
  if (status != QL_STATUS_OK) {
    goto cleanup;
  }

  status = build_inputs(query, problem, left_signature,
                        left_signature_view.argument_count, error);
  if (status != QL_STATUS_OK) {
    goto cleanup;
  }
  query->view.input_count = left_signature_view.argument_count;
  status = build_objects(query, left_ir, right_ir, &left_object_map,
                         &right_object_map, error);
  if (status != QL_STATUS_OK) {
    goto cleanup;
  }
  /* A finer memory observation than the final reachable state needs a write
     order or a trace this encoding has no term for. Refused, not narrowed. */
  if (query->object_count != 0u &&
      (problem_view.contract.observations & QL_OBSERVE_MEMORY) != 0u &&
      problem_view.contract.memory_observation !=
          QL_MEMORY_FINAL_REACHABLE_STATE) {
    ql_error_set(
        error, QL_STATUS_TYPE_MISMATCH,
        "the contract observes memory as an ordered write sequence or a full "
        "trace, and this miter states only the final reachable state");
    status = QL_STATUS_TYPE_MISMATCH;
    goto cleanup;
  }
  /* A body that calls threads a memory value even when it touches no
     object of its own, because the call may write memory. The memory is an
     array whatever the object table looks like, so the array theory is
     needed as soon as either side threads one. */
  left_threads_a_trace = ir_threads_a_trace(left_ir, NULL);
  right_threads_a_trace = ir_threads_a_trace(right_ir, NULL);
  if (query->object_count != 0u || left_threads_a_trace != 0 ||
      right_threads_a_trace != 0) {
    logic = QL_SOLVER_LOGIC_QF_ABV;
  }

  encoder.allocator = selected;
  encoder.error = error;
  encoder.inputs = query->inputs;
  encoder.input_count = query->view.input_count;
  encoder.objects = query->objects;
  encoder.object_count = query->object_count;
  buffer_init(&encoder.term, selected);
  status = ql_smt2_builder_create(selected, logic, &encoder.builder, error);
  if (status != QL_STATUS_OK) {
    goto cleanup;
  }

  status = side_prepare(&left, left_ir, 'l', selected, query->inputs,
                        query->view.input_count, query->objects,
                        query->object_count, left_object_map, error);
  if (status == QL_STATUS_OK) {
    status = side_prepare(&right, right_ir, 'r', selected, query->inputs,
                          query->view.input_count, query->objects,
                          query->object_count, right_object_map, error);
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
  if ((problem_view.contract.observations & QL_OBSERVE_EXTERNAL_CALLS) != 0u &&
      problem_view.contract.external_call_observation !=
          QL_EXTERNAL_CALLS_IGNORE &&
      problem_view.contract.external_call_observation !=
          QL_EXTERNAL_CALLS_ORDERED_TRACE) {
    ql_error_set(error, QL_STATUS_TYPE_MISMATCH,
                 "the contract observes external calls in a way this miter has "
                 "no term for");
    status = QL_STATUS_TYPE_MISMATCH;
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
                          query->view.input_count, query->objects,
                          query->object_count, &maximum_width, error);
  if (status == QL_STATUS_OK &&
      (left_threads_a_trace != 0 || right_threads_a_trace != 0)) {
    /* One history both sides start from, exactly as they start from one
       memory. The memory itself is declared with the object table when
       there is one; a body that only calls threads memory without owning
       any object, and still needs the one they share. */
    if (maximum_width < QL_PRODUCT_TRACE_WIDTH) {
      maximum_width = QL_PRODUCT_TRACE_WIDTH;
    }
    if (query->object_count == 0u) {
      status = ql_smt2_builder_declare_array(
          encoder.builder, QL_PRODUCT_MEMORY_SYMBOL, QL_PRODUCT_ADDRESS_WIDTH,
          QL_PRODUCT_BYTE_WIDTH, error);
    }
    if (status == QL_STATUS_OK) {
      status =
          ql_smt2_builder_declare_bv(encoder.builder, QL_PRODUCT_TRACE_SYMBOL,
                                     QL_PRODUCT_TRACE_WIDTH, error);
    }
  }
  if (status == QL_STATUS_OK) {
    status =
        encode_precondition(&encoder, left_signature, &problem_view.contract);
  }
  if (status == QL_STATUS_OK) {
    status = encode_side(&encoder, &left, selected);
  }
  if (status == QL_STATUS_OK) {
    status = encode_side(&encoder, &right, selected);
  }
  if (status == QL_STATUS_OK) {
    status = encode_assumptions(&encoder, &left, &right);
  }
  if (status == QL_STATUS_OK) {
    status = encode_observation_equality(
        &encoder, &problem_view.contract, left.return_kind != QL_IR_TYPE_VOID,
        (problem_view.contract.observations & QL_OBSERVE_EXTERNAL_CALLS) !=
                0u &&
            problem_view.contract.external_call_observation ==
                QL_EXTERNAL_CALLS_ORDERED_TRACE &&
            left_threads_a_trace != 0 && right_threads_a_trace != 0);
  }
  if (status == QL_STATUS_OK) {
    status = encode_domain(&encoder, &problem_view.contract);
  }
  if (status == QL_STATUS_OK) {
    status = encode_violation(&encoder, &problem_view.contract);
  }
  if (status == QL_STATUS_OK) {
    QL_STAGE_MARK(stage_smt2);
    status = ql_smt2_builder_build(encoder.builder, &query->prefix, error);
    if (status == QL_STATUS_OK) {
      status = ql_artifact_create(
          selected, QL_ARTIFACT_KIND_SMTLIB2, QL_SMTLIB2_SCHEMA_VERSION,
          product_violation_assertion, sizeof(product_violation_assertion) - 1u,
          &query->violation, error);
    }
    QL_STAGE_ADD(QL_STAGE_SMT2, stage_smt2);
  }
  if (status == QL_STATUS_OK && query->object_count != 0u) {
    buffer_reset(&encoder.term);
    status = term_add(&encoder, "(assert (and " QL_PRODUCT_VIOLATION_SYMBOL);
    for (index = 0u; index < query->object_count && status == QL_STATUS_OK;
         ++index) {
      status = term_addf(&encoder, " (bvule %s (_ bv%llu %u))",
                         query->objects[index].size_symbol,
                         (unsigned long long)QL_PRODUCT_REPLAYABLE_OBJECT_BYTES,
                         QL_PRODUCT_ADDRESS_WIDTH);
    }
    if (status == QL_STATUS_OK) {
      status = term_add(&encoder, "))\n");
    }
    if (status == QL_STATUS_OK) {
      QL_STAGE_MARK(stage_smt2);
      status = ql_artifact_create(selected, QL_ARTIFACT_KIND_SMTLIB2,
                                  QL_SMTLIB2_SCHEMA_VERSION,
                                  buffer_text(&encoder.term), encoder.term.size,
                                  &query->bounded_violation, error);
      QL_STAGE_ADD(QL_STAGE_SMT2, stage_smt2);
    }
  }
  if (status == QL_STATUS_OK) {
    QL_STAGE_MARK(stage_smt2);
    status = ql_artifact_create(
        selected, QL_ARTIFACT_KIND_SMTLIB2, QL_SMTLIB2_SCHEMA_VERSION,
        product_domain_assertion, sizeof(product_domain_assertion) - 1u,
        &query->domain, error);
    QL_STAGE_ADD(QL_STAGE_SMT2, stage_smt2);
  }
  if (status != QL_STATUS_OK) {
    goto cleanup;
  }

  query->view.struct_size = sizeof(query->view);
  query->view.schema_version = QL_PRODUCT_SCHEMA_VERSION;
  query->view.relation = problem_view.contract.relation;
  query->view.ub_policy = problem_view.contract.ub_policy;
  query->view.covered_observations = problem_view.contract.observations;
  query->view.logic = logic;
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
    status = ql_artifact_get_view(query->violation, &artifact_view, error);
  }
  if (status == QL_STATUS_OK) {
    query->view.violation_digest = artifact_view.digest;
    status = ql_artifact_get_view(query->domain, &artifact_view, error);
  }
  if (status == QL_STATUS_OK) {
    query->view.domain_digest = artifact_view.digest;
  }

cleanup:
  selected->deallocate(selected->user_data, left_object_map);
  selected->deallocate(selected->user_data, right_object_map);
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
    ql_error_set(error, QL_STATUS_NOT_FOUND, "product query has no input %zu",
                 index);
    return QL_STATUS_NOT_FOUND;
  }
  *output = query->inputs[index];
  ql_error_clear(error);
  return QL_STATUS_OK;
}

size_t QL_CALL ql_product_query_object_count(const ql_product_query *query) {
  return query == NULL ? 0u : query->object_count;
}

ql_status QL_CALL ql_product_query_object_at(const ql_product_query *query,
                                             size_t index,
                                             ql_product_object_v1 *output,
                                             ql_error *error) {
  if (query == NULL || output == NULL) {
    ql_error_set(error, QL_STATUS_INVALID_ARGUMENT,
                 "product query and object output are required");
    return QL_STATUS_INVALID_ARGUMENT;
  }
  if (index >= query->object_count) {
    ql_error_set(error, QL_STATUS_NOT_FOUND, "product query has no object %zu",
                 index);
    return QL_STATUS_NOT_FOUND;
  }
  memset(output, 0, sizeof(*output));
  output->struct_size = sizeof(*output);
  output->index = (uint32_t)index;
  output->address_width = QL_PRODUCT_ADDRESS_WIDTH;
  output->left_base_parameter = query->objects[index].left_base_parameter;
  output->right_base_parameter = query->objects[index].right_base_parameter;
  output->base_symbol = query->objects[index].base_symbol;
  output->size_symbol = query->objects[index].size_symbol;
  ql_error_clear(error);
  return QL_STATUS_OK;
}

const char *QL_CALL
ql_product_query_memory_symbol(const ql_product_query *query) {
  if (query == NULL || query->object_count == 0u) {
    return NULL;
  }
  return QL_PRODUCT_MEMORY_SYMBOL;
}

const ql_artifact *QL_CALL
ql_product_query_prefix_artifact(const ql_product_query *query) {
  return query == NULL ? NULL : query->prefix;
}

const ql_artifact *QL_CALL
ql_product_query_violation_artifact(const ql_product_query *query) {
  return query == NULL ? NULL : query->violation;
}

const ql_artifact *QL_CALL
ql_product_query_bounded_violation_artifact(const ql_product_query *query) {
  return query == NULL ? NULL : query->bounded_violation;
}

const ql_artifact *QL_CALL
ql_product_query_domain_artifact(const ql_product_query *query) {
  return query == NULL ? NULL : query->domain;
}
