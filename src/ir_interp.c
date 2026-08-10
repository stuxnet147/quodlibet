#include "quodlibet/ir_interp.h"

#include "quodlibet/allocator.h"

#include <string.h>

/* Restoring division shifts the running remainder one bit past the operand
   width, so the internal representation carries one word more than the
   advertised maximum width. */
#define INTERP_WORDS 5u

/* A call with more arguments than this is not run. Nothing this profile
   lowers comes close, and the bound keeps the target free of allocation. */
#define QL_IR_INTERP_MAX_ARGUMENTS 16u
/* A call may return state plus several scalar values. The bound keeps the
   callee bridge allocation-free while covering the public product fragment. */
#define QL_IR_INTERP_MAX_CALL_RESULTS 64u

typedef struct interp_bits {
  uint64_t words[INTERP_WORDS];
} interp_bits;

/* A memory version is the writes that led to it, newest first, over the
   objects' initial images. Storing versions this way keeps every older
   version readable, which the IR allows: a load may name a memory value that
   a later store has already superseded. */
typedef struct interp_store {
  const struct interp_store *previous;
  struct interp_store *allocation_next;
  uint64_t address;
  uint32_t width;
  uint8_t bytes[QL_IR_INTERP_VALUE_CAPACITY];
} interp_store;

typedef struct interp_value {
  interp_bits bits;
  const interp_store *store;
  uint32_t width;
  ql_ir_type_id type;
  ql_ir_type_kind kind;
  uint8_t defined;
  uint8_t bound;
} interp_value;

typedef struct interp_context {
  ql_allocator allocator;
  const ql_ir *ir;
  ql_ir_view_v1 view;
  ql_ir_interp_result_v1 *result;
  interp_value *values;
  interp_value *phi_snapshot;
  const interp_value *phi_inputs;
  const ql_ir_interp_object_v1 *objects;
  size_t object_count;
  const ql_ir_interp_callees_v1 *callees;
  uint64_t events;
  interp_store *allocations;
  const interp_store *newest_memory;
  const interp_store *final_memory;
  uint64_t step_limit;
} interp_context;

/* ------------------------------------------------------------------ */
/* fixed-capacity bit-vector arithmetic                                */
/* ------------------------------------------------------------------ */

static void bits_zero(interp_bits *value) { memset(value, 0, sizeof(*value)); }

static int bits_test(const interp_bits *value, uint32_t index) {
  if (index >= INTERP_WORDS * 64u) {
    return 0;
  }
  return (value->words[index / 64u] & (UINT64_C(1) << (index % 64u))) != 0u;
}

static void bits_set(interp_bits *value, uint32_t index) {
  if (index < INTERP_WORDS * 64u) {
    value->words[index / 64u] |= UINT64_C(1) << (index % 64u);
  }
}

static void bits_mask(interp_bits *value, uint32_t width) {
  size_t index;
  for (index = 0u; index < INTERP_WORDS; ++index) {
    uint32_t low = (uint32_t)index * 64u;
    if (width <= low) {
      value->words[index] = 0u;
    } else if (width < low + 64u) {
      value->words[index] &= (UINT64_C(1) << (width - low)) - UINT64_C(1);
    }
  }
}

static int bits_is_zero(const interp_bits *value) {
  size_t index;
  for (index = 0u; index < INTERP_WORDS; ++index) {
    if (value->words[index] != 0u) {
      return 0;
    }
  }
  return 1;
}

static int bits_equal(const interp_bits *left, const interp_bits *right) {
  size_t index;
  for (index = 0u; index < INTERP_WORDS; ++index) {
    if (left->words[index] != right->words[index]) {
      return 0;
    }
  }
  return 1;
}

static int bits_unsigned_less(const interp_bits *left,
                              const interp_bits *right) {
  size_t index;
  for (index = INTERP_WORDS; index-- > 0u;) {
    if (left->words[index] != right->words[index]) {
      return left->words[index] < right->words[index];
    }
  }
  return 0;
}

static void bits_not(const interp_bits *value, interp_bits *out) {
  size_t index;
  for (index = 0u; index < INTERP_WORDS; ++index) {
    out->words[index] = ~value->words[index];
  }
}

static void bits_add(const interp_bits *left, const interp_bits *right,
                     interp_bits *out) {
  uint64_t carry = 0u;
  size_t index;
  for (index = 0u; index < INTERP_WORDS; ++index) {
    uint64_t sum = left->words[index] + right->words[index];
    uint64_t next = sum < left->words[index] ? 1u : 0u;
    sum += carry;
    if (sum < carry) {
      next = 1u;
    }
    out->words[index] = sum;
    carry = next;
  }
}

static void bits_sub(const interp_bits *left, const interp_bits *right,
                     interp_bits *out) {
  uint64_t borrow = 0u;
  size_t index;
  for (index = 0u; index < INTERP_WORDS; ++index) {
    uint64_t high = left->words[index];
    uint64_t low = right->words[index];
    uint64_t difference = high - low;
    uint64_t next = high < low ? 1u : 0u;
    if (difference < borrow) {
      next = 1u;
    }
    difference -= borrow;
    out->words[index] = difference;
    borrow = next;
  }
}

static void bits_negate(const interp_bits *value, uint32_t width,
                        interp_bits *out) {
  interp_bits zero;
  bits_zero(&zero);
  bits_sub(&zero, value, out);
  bits_mask(out, width);
}

static void bits_shift_left(const interp_bits *value, uint32_t amount,
                            interp_bits *out) {
  interp_bits shifted;
  size_t word_shift = amount / 64u;
  uint32_t bit_shift = amount % 64u;
  size_t index;

  bits_zero(&shifted);
  if (word_shift < INTERP_WORDS) {
    for (index = INTERP_WORDS; index-- > word_shift;) {
      size_t source = index - word_shift;
      uint64_t result = value->words[source];
      if (bit_shift != 0u) {
        result <<= bit_shift;
        if (source > 0u) {
          result |= value->words[source - 1u] >> (64u - bit_shift);
        }
      }
      shifted.words[index] = result;
    }
  }
  *out = shifted;
}

static void bits_shift_right(const interp_bits *value, uint32_t amount,
                             interp_bits *out) {
  interp_bits shifted;
  size_t word_shift = amount / 64u;
  uint32_t bit_shift = amount % 64u;
  size_t index;

  bits_zero(&shifted);
  for (index = 0u; index + word_shift < INTERP_WORDS; ++index) {
    size_t source = index + word_shift;
    uint64_t result = value->words[source];
    if (bit_shift != 0u) {
      result >>= bit_shift;
      if (source + 1u < INTERP_WORDS) {
        result |= value->words[source + 1u] << (64u - bit_shift);
      }
    }
    shifted.words[index] = result;
  }
  *out = shifted;
}

/* Schoolbook multiplication over 32-bit limbs. Each accumulation step stays
   inside 64 bits, so no wider integer type is needed. */
static void bits_multiply(const interp_bits *left, const interp_bits *right,
                          interp_bits *out) {
  uint32_t left_limbs[INTERP_WORDS * 2u];
  uint32_t right_limbs[INTERP_WORDS * 2u];
  uint32_t product[INTERP_WORDS * 2u];
  size_t index;
  size_t inner;

  for (index = 0u; index < INTERP_WORDS; ++index) {
    left_limbs[index * 2u] = (uint32_t)(left->words[index] & 0xffffffffu);
    left_limbs[index * 2u + 1u] = (uint32_t)(left->words[index] >> 32u);
    right_limbs[index * 2u] = (uint32_t)(right->words[index] & 0xffffffffu);
    right_limbs[index * 2u + 1u] = (uint32_t)(right->words[index] >> 32u);
  }
  memset(product, 0, sizeof(product));
  for (index = 0u; index < INTERP_WORDS * 2u; ++index) {
    uint64_t carry = 0u;
    if (left_limbs[index] == 0u) {
      continue;
    }
    for (inner = 0u; index + inner < INTERP_WORDS * 2u; ++inner) {
      uint64_t accumulator =
          (uint64_t)product[index + inner] +
          (uint64_t)left_limbs[index] * (uint64_t)right_limbs[inner] + carry;
      product[index + inner] = (uint32_t)accumulator;
      carry = accumulator >> 32u;
    }
  }
  for (index = 0u; index < INTERP_WORDS; ++index) {
    out->words[index] = (uint64_t)product[index * 2u] |
                        ((uint64_t)product[index * 2u + 1u] << 32u);
  }
}

/* Restoring division. Every call site has already excluded a zero divisor. */
static void bits_unsigned_divide(const interp_bits *dividend,
                                 const interp_bits *divisor, uint32_t width,
                                 interp_bits *quotient,
                                 interp_bits *remainder) {
  interp_bits running_quotient;
  interp_bits running_remainder;
  uint32_t index;

  bits_zero(&running_quotient);
  bits_zero(&running_remainder);
  for (index = width; index-- > 0u;) {
    bits_shift_left(&running_remainder, 1u, &running_remainder);
    if (bits_test(dividend, index)) {
      bits_set(&running_remainder, 0u);
    }
    if (!bits_unsigned_less(&running_remainder, divisor)) {
      bits_sub(&running_remainder, divisor, &running_remainder);
      bits_set(&running_quotient, index);
    }
  }
  *quotient = running_quotient;
  *remainder = running_remainder;
}

static void bits_signed_divide(const interp_bits *dividend,
                               const interp_bits *divisor, uint32_t width,
                               interp_bits *quotient, interp_bits *remainder) {
  int dividend_negative = bits_test(dividend, width - 1u);
  int divisor_negative = bits_test(divisor, width - 1u);
  interp_bits left = *dividend;
  interp_bits right = *divisor;

  if (dividend_negative) {
    bits_negate(&left, width, &left);
  }
  if (divisor_negative) {
    bits_negate(&right, width, &right);
  }
  bits_unsigned_divide(&left, &right, width, quotient, remainder);
  if (dividend_negative != divisor_negative) {
    bits_negate(quotient, width, quotient);
  }
  /* C truncates toward zero, so the remainder keeps the dividend's sign. */
  if (dividend_negative) {
    bits_negate(remainder, width, remainder);
  }
}

static int bits_exceeds_word(const interp_bits *value) {
  size_t index;
  for (index = 1u; index < INTERP_WORDS; ++index) {
    if (value->words[index] != 0u) {
      return 1;
    }
  }
  return 0;
}

/* ------------------------------------------------------------------ */
/* byte encoding                                                       */
/* ------------------------------------------------------------------ */

static size_t interp_encoded_size(uint32_t width) {
  return (size_t)(width / 8u) + (width % 8u == 0u ? 0u : 1u);
}

static int interp_bits_from_bytes(const void *data, size_t size, uint32_t width,
                                  interp_bits *out) {
  const uint8_t *bytes = (const uint8_t *)data;
  size_t index;

  if (data == NULL || width == 0u || width > QL_IR_INTERP_MAX_BIT_WIDTH ||
      size != interp_encoded_size(width)) {
    return 0;
  }
  bits_zero(out);
  for (index = 0u; index < size; ++index) {
    out->words[index / 8u] |= (uint64_t)bytes[index] << ((index % 8u) * 8u);
  }
  if (width % 8u != 0u && (bytes[size - 1u] >> (width % 8u)) != 0u) {
    return 0;
  }
  return 1;
}

static void interp_bits_to_bytes(const interp_bits *value, uint32_t width,
                                 uint8_t *bytes, size_t *size) {
  size_t encoded = interp_encoded_size(width);
  size_t index;

  for (index = 0u; index < encoded; ++index) {
    bytes[index] =
        (uint8_t)((value->words[index / 8u] >> ((index % 8u) * 8u)) & 0xffu);
  }
  *size = encoded;
}

/* ------------------------------------------------------------------ */
/* memory                                                              */
/* ------------------------------------------------------------------ */

static size_t interp_byte_width(uint32_t bit_width) {
  return (size_t)(bit_width / 8u) + (bit_width % 8u == 0u ? 0u : 1u);
}

/* Natural alignment on the target ABI: a scalar of N bytes is N-aligned when
   N is a power of two up to sixteen. A width with no such alignment is not a
   C scalar layout, so nothing is required of it. */
static uint32_t interp_natural_alignment(size_t byte_width) {
  if (byte_width == 0u || byte_width > 16u ||
      (byte_width & (byte_width - 1u)) != 0u) {
    return 1u;
  }
  return (uint32_t)byte_width;
}

/* The whole access must lie inside one live object and be naturally aligned.
   Nothing below the first object address belongs to any object, so a null
   dereference fails here rather than reading a zero page. */
static int interp_access_defined(const interp_context *context,
                                 uint64_t address, size_t byte_width) {
  uint32_t alignment = interp_natural_alignment(byte_width);
  size_t index;

  if (alignment > 1u && (address % (uint64_t)alignment) != 0u) {
    return 0;
  }
  for (index = 0u; index < context->object_count; ++index) {
    const ql_ir_interp_object_v1 *object = &context->objects[index];
    uint64_t offset;
    if (address < object->base) {
      continue;
    }
    offset = address - object->base;
    if (offset <= object->size &&
        object->size - offset >= (uint64_t)byte_width) {
      return 1;
    }
  }
  return 0;
}

static int interp_initial_byte(const interp_context *context, uint64_t address,
                               uint8_t *output) {
  size_t index;

  for (index = 0u; index < context->object_count; ++index) {
    const ql_ir_interp_object_v1 *object = &context->objects[index];
    uint64_t offset;
    if (address < object->base) {
      continue;
    }
    offset = address - object->base;
    if (offset >= object->size) {
      continue;
    }
    *output = object->initial != NULL
                  ? ((const uint8_t *)object->initial)[offset]
                  : 0u;
    return 1;
  }
  return 0;
}

static int interp_read_byte(const interp_context *context,
                            const interp_store *version, uint64_t address,
                            uint8_t *output) {
  const interp_store *cursor;

  for (cursor = version; cursor != NULL; cursor = cursor->previous) {
    uint64_t offset;
    if (address < cursor->address) {
      continue;
    }
    offset = address - cursor->address;
    if (offset < (uint64_t)cursor->width) {
      *output = cursor->bytes[offset];
      return 1;
    }
  }
  return interp_initial_byte(context, address, output);
}

static interp_store *interp_push_store(interp_context *context,
                                       const interp_store *previous,
                                       uint64_t address, size_t byte_width,
                                       const interp_bits *value) {
  interp_store *record = (interp_store *)context->allocator.allocate(
      context->allocator.user_data, sizeof(*record));
  size_t index;

  if (record == NULL) {
    return NULL;
  }
  memset(record, 0, sizeof(*record));
  record->previous = previous;
  record->address = address;
  record->width = (uint32_t)byte_width;
  for (index = 0u; index < byte_width && index < sizeof(record->bytes);
       ++index) {
    record->bytes[index] =
        (uint8_t)((value->words[index / 8u] >> ((index % 8u) * 8u)) & 0xffu);
  }
  record->allocation_next = context->allocations;
  context->allocations = record;
  context->newest_memory = record;
  return record;
}

static void interp_release_stores(interp_context *context) {
  interp_store *cursor = context->allocations;
  while (cursor != NULL) {
    interp_store *next = cursor->allocation_next;
    context->allocator.deallocate(context->allocator.user_data, cursor);
    cursor = next;
  }
  context->allocations = NULL;
}

static void interp_write_final_images(interp_context *context,
                                      const interp_store *version) {
  size_t index;

  for (index = 0u; index < context->object_count; ++index) {
    const ql_ir_interp_object_v1 *object = &context->objects[index];
    uint8_t *destination = (uint8_t *)object->final_image;
    uint64_t offset;
    if (destination == NULL) {
      continue;
    }
    for (offset = 0u; offset < object->size; ++offset) {
      uint8_t byte = 0u;
      (void)interp_read_byte(context, version, object->base + offset, &byte);
      destination[offset] = byte;
    }
  }
}

/* The model's three standing constraints. Refusing a layout that breaks them
   is not pedantry: every access-definedness answer below assumes them, and so
   does the SMT side that has to agree with this one. */
static ql_status interp_validate_objects(const interp_context *context,
                                         ql_error *error) {
  size_t index;
  size_t other;

  if (context->object_count != 0u && context->objects == NULL) {
    ql_error_set(error, QL_STATUS_INVALID_ARGUMENT,
                 "interpreter object count is non-zero with no table");
    return QL_STATUS_INVALID_ARGUMENT;
  }
  for (index = 0u; index < context->object_count; ++index) {
    const ql_ir_interp_object_v1 *object = &context->objects[index];
    if (object->struct_size != 0u && object->struct_size < sizeof(*object)) {
      ql_error_set(error, QL_STATUS_ABI_MISMATCH,
                   "interpreter object structure is too small");
      return QL_STATUS_ABI_MISMATCH;
    }
    if (object->size == 0u) {
      ql_error_set(error, QL_STATUS_INVALID_ARGUMENT,
                   "interpreter object %zu is empty", index);
      return QL_STATUS_INVALID_ARGUMENT;
    }
    if (object->base < QL_IR_INTERP_FIRST_OBJECT_ADDRESS) {
      ql_error_set(error, QL_STATUS_INVALID_ARGUMENT,
                   "interpreter object %zu starts below the first "
                   "object address, which would put a valid access at "
                   "a null pointer",
                   index);
      return QL_STATUS_INVALID_ARGUMENT;
    }
    if (object->size > UINT64_MAX - object->base) {
      ql_error_set(error, QL_STATUS_INVALID_ARGUMENT,
                   "interpreter object %zu wraps the address space", index);
      return QL_STATUS_INVALID_ARGUMENT;
    }
    for (other = 0u; other < index; ++other) {
      const ql_ir_interp_object_v1 *previous = &context->objects[other];
      if (object->base < previous->base + previous->size &&
          previous->base < object->base + object->size) {
        ql_error_set(error, QL_STATUS_INVALID_ARGUMENT,
                     "interpreter objects %zu and %zu overlap", other, index);
        return QL_STATUS_INVALID_ARGUMENT;
      }
    }
  }
  return QL_STATUS_OK;
}

/* ------------------------------------------------------------------ */
/* small accessors                                                     */
/* ------------------------------------------------------------------ */

void QL_CALL ql_ir_interp_object_init(ql_ir_interp_object_v1 *object) {
  if (object == NULL) {
    return;
  }
  memset(object, 0, sizeof(*object));
  object->struct_size = sizeof(*object);
}

void QL_CALL ql_ir_interp_options_init(ql_ir_interp_options_v1 *options) {
  if (options == NULL) {
    return;
  }
  memset(options, 0, sizeof(*options));
  options->struct_size = sizeof(*options);
  options->schema_version = QL_IR_INTERP_SCHEMA_VERSION;
}

void QL_CALL ql_ir_interp_input_init(ql_ir_interp_input_v1 *input) {
  if (input == NULL) {
    return;
  }
  memset(input, 0, sizeof(*input));
  input->struct_size = sizeof(*input);
  input->value = QL_IR_INVALID_VALUE_ID;
}

const char *QL_CALL ql_ir_interp_outcome_string(ql_ir_interp_outcome outcome) {
  switch (outcome) {
  case QL_IR_INTERP_OUTCOME_RETURN:
    return "return";
  case QL_IR_INTERP_OUTCOME_TRAP:
    return "trap";
  case QL_IR_INTERP_OUTCOME_TERMINATE:
    return "terminate";
  case QL_IR_INTERP_OUTCOME_UNDEFINED_BEHAVIOR:
    return "undefined-behavior";
  case QL_IR_INTERP_OUTCOME_DIVERGE:
    return "diverge";
  case QL_IR_INTERP_OUTCOME_ASSUMPTION_VIOLATED:
    return "assumption-violated";
  case QL_IR_INTERP_OUTCOME_UNSUPPORTED:
    return "unsupported";
  case QL_IR_INTERP_OUTCOME_STEP_LIMIT:
    return "step-limit";
  default:
    return "unknown";
  }
}

const char *QL_CALL
ql_ir_interp_ub_reason_string(ql_ir_interp_ub_reason reason) {
  switch (reason) {
  case QL_IR_INTERP_UB_NONE:
    return "none";
  case QL_IR_INTERP_UB_GUARD_FAILED:
    return "guard-failed";
  case QL_IR_INTERP_UB_GUARD_UNDEFINED:
    return "guard-undefined";
  case QL_IR_INTERP_UB_GUARD_INSUFFICIENT:
    return "guard-insufficient";
  case QL_IR_INTERP_UB_TERMINATOR:
    return "terminator";
  default:
    return "unknown";
  }
}

/* ------------------------------------------------------------------ */
/* execution                                                           */
/* ------------------------------------------------------------------ */

static int interp_stop(interp_context *context, ql_ir_interp_outcome outcome,
                       ql_ir_interp_ub_reason reason, ql_ir_block_id block,
                       ql_ir_instruction_id instruction) {
  context->result->outcome = outcome;
  context->result->ub_reason = reason;
  context->result->block = block;
  context->result->instruction = instruction;
  return 0;
}

static int
interp_execute_instruction(interp_context *context,
                           const ql_ir_instruction_view_v1 *instruction,
                           ql_ir_block_id block, ql_ir_instruction_id id,
                           ql_ir_block_id previous_block) {
  interp_value result;
  const interp_value *left = NULL;
  const interp_value *right = NULL;
  uint32_t result_width = 0u;
  uint32_t operand_width = 0u;
  int any_undefined = 0;
  size_t index;

  memset(&result, 0, sizeof(result));
  result.defined = 1u;
  result.bound = 1u;

  for (index = 0u; index < instruction->operand_count; ++index) {
    if (context->values[instruction->operands[index]].defined == 0u) {
      any_undefined = 1;
    }
  }
  if (instruction->operand_count > 0u) {
    left = &context->values[instruction->operands[0]];
    operand_width = left->width;
  }
  if (instruction->operand_count > 1u) {
    right = &context->values[instruction->operands[1]];
  }
  if (instruction->result_count > 0u) {
    const interp_value *slot = &context->values[instruction->results[0]];
    result_width = slot->width;
    result.width = slot->width;
    result.type = slot->type;
    result.kind = slot->kind;
  }

  switch (instruction->opcode) {
  case QL_IR_OPCODE_IDENTITY:
    result.bits = left->bits;
    result.store = left->store;
    result.defined = left->defined;
    break;
  case QL_IR_OPCODE_PHI: {
    size_t chosen = instruction->block_operand_count;
    const interp_value *incoming;
    for (index = 0u; index < instruction->block_operand_count; ++index) {
      if (instruction->block_operands[index] == previous_block) {
        chosen = index;
        break;
      }
    }
    if (chosen == instruction->block_operand_count) {
      return interp_stop(context, QL_IR_INTERP_OUTCOME_UNSUPPORTED,
                         QL_IR_INTERP_UB_NONE, block, id);
    }
    incoming = &(context->phi_inputs != NULL
                     ? context->phi_inputs
                     : context->values)[instruction->operands[chosen]];
    result.bits = incoming->bits;
    result.store = incoming->store;
    result.defined = incoming->defined;
    break;
  }
  case QL_IR_OPCODE_SELECT: {
    /* Only the condition and the selected arm matter. The lowering
       evaluates both arms of `&&` and `||` eagerly and short-circuits
       definedness alone, so propagating from the arm that was not taken
       would invent undefined behaviour the C does not have. */
    const interp_value *chosen;
    if (left->defined == 0u) {
      result.defined = 0u;
      break;
    }
    chosen = bits_is_zero(&left->bits)
                 ? &context->values[instruction->operands[2]]
                 : &context->values[instruction->operands[1]];
    result.bits = chosen->bits;
    result.store = chosen->store;
    result.defined = chosen->defined;
    break;
  }
  case QL_IR_OPCODE_BOOL_NOT:
    if (any_undefined) {
      result.defined = 0u;
      break;
    }
    if (bits_is_zero(&left->bits)) {
      bits_set(&result.bits, 0u);
    }
    break;
  case QL_IR_OPCODE_BV_NOT:
    if (any_undefined) {
      result.defined = 0u;
      break;
    }
    bits_not(&left->bits, &result.bits);
    bits_mask(&result.bits, result_width);
    break;
  case QL_IR_OPCODE_BV_NEG:
    if (any_undefined) {
      result.defined = 0u;
      break;
    }
    bits_negate(&left->bits, result_width, &result.bits);
    break;
  case QL_IR_OPCODE_ADD:
    if (any_undefined) {
      result.defined = 0u;
      break;
    }
    bits_add(&left->bits, &right->bits, &result.bits);
    bits_mask(&result.bits, result_width);
    break;
  case QL_IR_OPCODE_SUB:
    if (any_undefined) {
      result.defined = 0u;
      break;
    }
    bits_sub(&left->bits, &right->bits, &result.bits);
    bits_mask(&result.bits, result_width);
    break;
  case QL_IR_OPCODE_MUL:
    if (any_undefined) {
      result.defined = 0u;
      break;
    }
    bits_multiply(&left->bits, &right->bits, &result.bits);
    bits_mask(&result.bits, result_width);
    break;
  case QL_IR_OPCODE_BV_AND:
  case QL_IR_OPCODE_BV_OR:
  case QL_IR_OPCODE_BV_XOR: {
    size_t word;
    if (any_undefined) {
      result.defined = 0u;
      break;
    }
    for (word = 0u; word < INTERP_WORDS; ++word) {
      uint64_t high = left->bits.words[word];
      uint64_t low = right->bits.words[word];
      result.bits.words[word] =
          instruction->opcode == QL_IR_OPCODE_BV_AND
              ? (high & low)
              : (instruction->opcode == QL_IR_OPCODE_BV_OR ? (high | low)
                                                           : (high ^ low));
    }
    bits_mask(&result.bits, result_width);
    break;
  }
  case QL_IR_OPCODE_UDIV:
  case QL_IR_OPCODE_SDIV:
  case QL_IR_OPCODE_UREM:
  case QL_IR_OPCODE_SREM: {
    interp_bits quotient;
    interp_bits remainder;
    const int is_signed = instruction->opcode == QL_IR_OPCODE_SDIV ||
                          instruction->opcode == QL_IR_OPCODE_SREM;
    const int wants_remainder = instruction->opcode == QL_IR_OPCODE_UREM ||
                                instruction->opcode == QL_IR_OPCODE_SREM;
    if (any_undefined || bits_is_zero(&right->bits)) {
      result.defined = 0u;
      break;
    }
    if (is_signed) {
      interp_bits minimum;
      interp_bits all_ones;
      bits_zero(&minimum);
      bits_set(&minimum, result_width - 1u);
      bits_zero(&all_ones);
      bits_not(&all_ones, &all_ones);
      bits_mask(&all_ones, result_width);
      /* The quotient of the most negative value by minus one is not
         representable, which C leaves undefined. */
      if (bits_equal(&left->bits, &minimum) &&
          bits_equal(&right->bits, &all_ones)) {
        result.defined = 0u;
        break;
      }
      bits_signed_divide(&left->bits, &right->bits, result_width, &quotient,
                         &remainder);
    } else {
      bits_unsigned_divide(&left->bits, &right->bits, result_width, &quotient,
                           &remainder);
    }
    result.bits = wants_remainder ? remainder : quotient;
    bits_mask(&result.bits, result_width);
    break;
  }
  case QL_IR_OPCODE_SHL:
  case QL_IR_OPCODE_LSHR:
  case QL_IR_OPCODE_ASHR: {
    uint64_t amount;
    if (any_undefined || bits_exceeds_word(&right->bits)) {
      result.defined = 0u;
      break;
    }
    amount = right->bits.words[0];
    if (amount >= (uint64_t)result_width) {
      result.defined = 0u;
      break;
    }
    if (instruction->opcode == QL_IR_OPCODE_SHL) {
      bits_shift_left(&left->bits, (uint32_t)amount, &result.bits);
    } else {
      bits_shift_right(&left->bits, (uint32_t)amount, &result.bits);
      if (instruction->opcode == QL_IR_OPCODE_ASHR &&
          bits_test(&left->bits, result_width - 1u)) {
        uint32_t bit;
        for (bit = result_width - (uint32_t)amount; bit < result_width; ++bit) {
          bits_set(&result.bits, bit);
        }
      }
    }
    bits_mask(&result.bits, result_width);
    break;
  }
  case QL_IR_OPCODE_EQ:
  case QL_IR_OPCODE_NE:
    if (any_undefined) {
      result.defined = 0u;
      break;
    }
    if (bits_equal(&left->bits, &right->bits) ==
        (instruction->opcode == QL_IR_OPCODE_EQ)) {
      bits_set(&result.bits, 0u);
    }
    break;
  case QL_IR_OPCODE_ULT:
  case QL_IR_OPCODE_ULE:
    if (any_undefined) {
      result.defined = 0u;
      break;
    }
    if (bits_unsigned_less(&left->bits, &right->bits) ||
        (instruction->opcode == QL_IR_OPCODE_ULE &&
         bits_equal(&left->bits, &right->bits))) {
      bits_set(&result.bits, 0u);
    }
    break;
  case QL_IR_OPCODE_SLT:
  case QL_IR_OPCODE_SLE: {
    int left_negative;
    int right_negative;
    int less;
    if (any_undefined) {
      result.defined = 0u;
      break;
    }
    left_negative = bits_test(&left->bits, operand_width - 1u);
    right_negative = bits_test(&right->bits, operand_width - 1u);
    less = left_negative != right_negative
               ? left_negative
               : bits_unsigned_less(&left->bits, &right->bits);
    if (less || (instruction->opcode == QL_IR_OPCODE_SLE &&
                 bits_equal(&left->bits, &right->bits))) {
      bits_set(&result.bits, 0u);
    }
    break;
  }
  case QL_IR_OPCODE_ZEXT:
    if (any_undefined) {
      result.defined = 0u;
      break;
    }
    result.bits = left->bits;
    break;
  case QL_IR_OPCODE_SEXT:
    if (any_undefined) {
      result.defined = 0u;
      break;
    }
    result.bits = left->bits;
    if (bits_test(&left->bits, operand_width - 1u)) {
      uint32_t bit;
      for (bit = operand_width; bit < result_width; ++bit) {
        bits_set(&result.bits, bit);
      }
    }
    bits_mask(&result.bits, result_width);
    break;
  case QL_IR_OPCODE_TRUNC:
    if (any_undefined) {
      result.defined = 0u;
      break;
    }
    result.bits = left->bits;
    bits_mask(&result.bits, result_width);
    break;
  case QL_IR_OPCODE_PTR_ADD:
    if (any_undefined) {
      result.defined = 0u;
      break;
    }
    if (right->width != result_width) {
      /* Schema v1 only says the offset is a bit-vector. Rather than
         invent a widening rule the SMT side would have to match, this
         refuses an offset that is not already pointer-width. */
      return interp_stop(context, QL_IR_INTERP_OUTCOME_UNSUPPORTED,
                         QL_IR_INTERP_UB_NONE, block, id);
    }
    bits_add(&left->bits, &right->bits, &result.bits);
    bits_mask(&result.bits, result_width);
    break;
  case QL_IR_OPCODE_PTR_TO_BV:
  case QL_IR_OPCODE_BV_TO_PTR:
    /* The type rule already fixes both sides to the same width, so this
       is a reinterpretation and nothing else. */
    if (any_undefined) {
      result.defined = 0u;
      break;
    }
    result.bits = left->bits;
    break;
  case QL_IR_OPCODE_LOAD: {
    size_t byte_width = interp_byte_width(result_width);
    uint64_t address;
    size_t byte;
    if (left->defined == 0u ||
        context->values[instruction->operands[1]].defined == 0u) {
      result.defined = 0u;
      break;
    }
    address = context->values[instruction->operands[1]].bits.words[0];
    if (bits_exceeds_word(&context->values[instruction->operands[1]].bits) ||
        !interp_access_defined(context, address, byte_width)) {
      result.defined = 0u;
      break;
    }
    bits_zero(&result.bits);
    for (byte = 0u; byte < byte_width; ++byte) {
      uint8_t value = 0u;
      (void)interp_read_byte(context, left->store, address + (uint64_t)byte,
                             &value);
      result.bits.words[byte / 8u] |= (uint64_t)value << ((byte % 8u) * 8u);
    }
    bits_mask(&result.bits, result_width);
    break;
  }
  case QL_IR_OPCODE_MEMORY_IMAGE: {
    const interp_value *pointer = &context->values[instruction->operands[1]];
    uint64_t address;
    size_t byte;
    int holds = 1;
    if (left->defined == 0u || pointer->defined == 0u) {
      result.defined = 0u;
      break;
    }
    address = pointer->bits.words[0];
    /* The bytes have to be somewhere an access could reach them. An
       image of bytes outside every live object is a claim about storage
       that does not exist, which is false rather than unmodelled. */
    if (bits_exceeds_word(&pointer->bits) ||
        !interp_access_defined(context, address, 1u) ||
        !interp_access_defined(
            context, address + (uint64_t)(instruction->image_size - 1u), 1u)) {
      holds = 0;
    }
    for (byte = 0u; holds != 0 && byte < instruction->image_size; ++byte) {
      uint8_t value = 0u;
      (void)interp_read_byte(context, left->store, address + (uint64_t)byte,
                             &value);
      if (value != ((const uint8_t *)instruction->image)[byte]) {
        holds = 0;
      }
    }
    bits_zero(&result.bits);
    if (holds != 0) {
      result.bits.words[0] = 1u;
    }
    break;
  }
  case QL_IR_OPCODE_STORE: {
    const interp_value *pointer = &context->values[instruction->operands[1]];
    const interp_value *stored = &context->values[instruction->operands[2]];
    size_t byte_width = interp_byte_width(stored->width);
    uint64_t address;
    interp_store *record;
    /* A store of a value a partial operation left undefined makes the
       whole memory version undefined. Tracking undefinedness per byte
       would be more precise; this is the conservative direction, and the
       observation that reads it still reports the guard as too weak. */
    if (left->defined == 0u || pointer->defined == 0u ||
        stored->defined == 0u) {
      result.defined = 0u;
      break;
    }
    address = pointer->bits.words[0];
    if (bits_exceeds_word(&pointer->bits) ||
        !interp_access_defined(context, address, byte_width)) {
      result.defined = 0u;
      break;
    }
    record = interp_push_store(context, left->store, address, byte_width,
                               &stored->bits);
    if (record == NULL) {
      return interp_stop(context, QL_IR_INTERP_OUTCOME_UNSUPPORTED,
                         QL_IR_INTERP_UB_NONE, block, id);
    }
    result.store = record;
    break;
  }
  case QL_IR_OPCODE_CALL: {
    /* The IR leaves an external callee uninterpreted, so the only thing
       that can say what it returns is the specification the caller
       supplied. Without one there is nothing honest to do but stop. */
    ql_ir_interp_argument_v1 arguments[QL_IR_INTERP_MAX_ARGUMENTS];
    uint8_t argument_bytes[QL_IR_INTERP_MAX_ARGUMENTS]
                          [QL_IR_INTERP_VALUE_CAPACITY];
    uint8_t result_bytes[QL_IR_INTERP_MAX_CALL_RESULTS *
                         QL_IR_INTERP_VALUE_CAPACITY];
    size_t result_offsets[QL_IR_INTERP_MAX_CALL_RESULTS];
    size_t result_sizes[QL_IR_INTERP_MAX_CALL_RESULTS];
    size_t argument_count = 0u;
    size_t state_operands = 0u;
    size_t result_size = 0u;
    size_t operand;
    size_t slot;

    if (context->callees == NULL || context->callees->invoke == NULL) {
      return interp_stop(context, QL_IR_INTERP_OUTCOME_UNSUPPORTED,
                         QL_IR_INTERP_UB_NONE, block, id);
    }
    /* Leading memory and event-trace operands are the states the call
       threads, not arguments. */
    for (operand = 0u; operand < instruction->operand_count; ++operand) {
      const interp_value *value =
          &context->values[instruction->operands[operand]];
      if (value->kind != QL_IR_TYPE_MEMORY &&
          value->kind != QL_IR_TYPE_EVENT_TRACE) {
        break;
      }
      ++state_operands;
    }
    for (operand = state_operands; operand < instruction->operand_count;
         ++operand) {
      const interp_value *value =
          &context->values[instruction->operands[operand]];
      size_t width;
      if (argument_count >= QL_IR_INTERP_MAX_ARGUMENTS) {
        return interp_stop(context, QL_IR_INTERP_OUTCOME_UNSUPPORTED,
                           QL_IR_INTERP_UB_NONE, block, id);
      }
      if (value->defined == 0u) {
        /* Handing an undefined value to something observable is an
           observation of it. */
        return interp_stop(context, QL_IR_INTERP_OUTCOME_UNDEFINED_BEHAVIOR,
                           QL_IR_INTERP_UB_GUARD_INSUFFICIENT, block, id);
      }
      interp_bits_to_bytes(&value->bits, value->width,
                           argument_bytes[argument_count], &width);
      memset(&arguments[argument_count], 0, sizeof(arguments[argument_count]));
      arguments[argument_count].struct_size = sizeof(arguments[argument_count]);
      arguments[argument_count].bit_width = value->width;
      arguments[argument_count].size = width;
      arguments[argument_count].data = argument_bytes[argument_count];
      ++argument_count;
    }
    if (instruction->result_count > QL_IR_INTERP_MAX_CALL_RESULTS) {
      return interp_stop(context, QL_IR_INTERP_OUTCOME_UNSUPPORTED,
                         QL_IR_INTERP_UB_NONE, block, id);
    }
    /* State results are threaded by the interpreter. Every other result is
       supplied by the callee in one tightly packed little-endian byte run. */
    for (slot = 0u; slot < instruction->result_count; ++slot) {
      const interp_value *target = &context->values[instruction->results[slot]];
      size_t width;
      result_offsets[slot] = 0u;
      result_sizes[slot] = 0u;
      if (target->kind == QL_IR_TYPE_MEMORY ||
          target->kind == QL_IR_TYPE_EVENT_TRACE) {
        continue;
      }
      width = interp_byte_width(target->width);
      if (width > QL_IR_INTERP_VALUE_CAPACITY ||
          result_size > sizeof(result_bytes) - width) {
        return interp_stop(context, QL_IR_INTERP_OUTCOME_UNSUPPORTED,
                           QL_IR_INTERP_UB_NONE, block, id);
      }
      result_offsets[slot] = result_size;
      result_sizes[slot] = width;
      result_size += width;
    }
    memset(result_bytes, 0, sizeof(result_bytes));
    {
      if (!context->callees->invoke(
              context->callees->user_data, instruction->symbol, arguments,
              argument_count, result_bytes, result_size)) {
        return interp_stop(context, QL_IR_INTERP_OUTCOME_UNSUPPORTED,
                           QL_IR_INTERP_UB_NONE, block, id);
      }
      ++context->events;
      for (slot = 0u; slot < instruction->result_count; ++slot) {
        interp_value *target = &context->values[instruction->results[slot]];
        if (target->kind == QL_IR_TYPE_MEMORY) {
          /* The specification asserted this callee does not write
             memory, so the version it hands on is the one it was
             given. */
          size_t which;
          target->store = NULL;
          for (which = 0u; which < state_operands; ++which) {
            const interp_value *source =
                &context->values[instruction->operands[which]];
            if (source->kind == QL_IR_TYPE_MEMORY) {
              target->store = source->store;
              break;
            }
          }
          target->defined = 1u;
        } else if (target->kind == QL_IR_TYPE_EVENT_TRACE) {
          target->defined = 1u;
        } else {
          if (!interp_bits_from_bytes(result_bytes + result_offsets[slot],
                                      result_sizes[slot], target->width,
                                      &target->bits)) {
            return interp_stop(context, QL_IR_INTERP_OUTCOME_UNSUPPORTED,
                               QL_IR_INTERP_UB_NONE, block, id);
          }
          target->defined = 1u;
        }
        target->bound = 1u;
      }
    }
    return 1;
  }
  case QL_IR_OPCODE_ASSUME:
    if (left->defined == 0u || bits_is_zero(&left->bits)) {
      return interp_stop(context, QL_IR_INTERP_OUTCOME_ASSUMPTION_VIOLATED,
                         QL_IR_INTERP_UB_NONE, block, id);
    }
    return 1;
  case QL_IR_OPCODE_UB_GUARD:
    if (left->defined == 0u) {
      return interp_stop(context, QL_IR_INTERP_OUTCOME_UNDEFINED_BEHAVIOR,
                         QL_IR_INTERP_UB_GUARD_UNDEFINED, block, id);
    }
    if (bits_is_zero(&left->bits)) {
      return interp_stop(context, QL_IR_INTERP_OUTCOME_UNDEFINED_BEHAVIOR,
                         QL_IR_INTERP_UB_GUARD_FAILED, block, id);
    }
    return 1;
  default:
    /* Memory, calls, event traces, pointers, and floating point are not
       modelled yet. Guessing a semantics here would be a wrong answer
       everywhere, so the run says it does not know. */
    return interp_stop(context, QL_IR_INTERP_OUTCOME_UNSUPPORTED,
                       QL_IR_INTERP_UB_NONE, block, id);
  }

  if (instruction->result_count == 1u) {
    interp_value *slot = &context->values[instruction->results[0]];
    result.width = slot->width;
    result.type = slot->type;
    result.kind = slot->kind;
    *slot = result;
  }
  return 1;
}

static int interp_observe(interp_context *context, ql_ir_value_id value,
                          ql_ir_block_id block) {
  if (value == QL_IR_INVALID_VALUE_ID || context->values[value].defined != 0u) {
    return 1;
  }
  /* Every guard on this path passed and an observation still read a value a
     partial operation left undefined, so the guard predicate was too weak.
     This is the sufficiency check the verifier cannot make structurally. */
  return interp_stop(context, QL_IR_INTERP_OUTCOME_UNDEFINED_BEHAVIOR,
                     QL_IR_INTERP_UB_GUARD_INSUFFICIENT, block,
                     QL_IR_INVALID_INSTRUCTION_ID);
}

static void interp_record_value(interp_context *context, ql_ir_value_id value) {
  const interp_value *slot = &context->values[value];
  context->result->has_value = 1u;
  context->result->value_type = slot->type;
  interp_bits_to_bytes(&slot->bits, slot->width, context->result->value,
                       &context->result->value_size);
}

static ql_status interp_execute(interp_context *context, ql_error *error) {
  ql_ir_block_id current = context->view.entry_block;
  ql_ir_block_id previous = QL_IR_INVALID_BLOCK_ID;
  uint64_t steps = 0u;

  for (;;) {
    ql_ir_block_view_v1 block;
    size_t index;
    int in_phis = 1;

    memset(&block, 0, sizeof(block));
    block.struct_size = sizeof(block);
    if (ql_ir_block_at(context->ir, current, &block, error) != QL_STATUS_OK) {
      return QL_STATUS_INVALID_ARGUMENT;
    }
    for (index = 0u; index < block.instruction_count; ++index) {
      ql_ir_instruction_view_v1 instruction;
      if (++steps > context->step_limit) {
        context->result->steps = steps;
        (void)interp_stop(context, QL_IR_INTERP_OUTCOME_STEP_LIMIT,
                          QL_IR_INTERP_UB_NONE, current,
                          QL_IR_INVALID_INSTRUCTION_ID);
        return QL_STATUS_OK;
      }
      memset(&instruction, 0, sizeof(instruction));
      instruction.struct_size = sizeof(instruction);
      if (ql_ir_instruction_at(context->ir, block.instructions[index],
                               &instruction, error) != QL_STATUS_OK) {
        return QL_STATUS_INVALID_ARGUMENT;
      }
      if (instruction.opcode == QL_IR_OPCODE_PHI && in_phis != 0) {
        if (context->phi_inputs == NULL) {
          memcpy(context->phi_snapshot, context->values,
                 context->view.value_count * sizeof(*context->phi_snapshot));
          context->phi_inputs = context->phi_snapshot;
        }
      } else {
        in_phis = 0;
        context->phi_inputs = NULL;
      }
      if (!interp_execute_instruction(context, &instruction, current,
                                      block.instructions[index], previous)) {
        context->result->steps = steps;
        return QL_STATUS_OK;
      }
    }
    context->phi_inputs = NULL;
    /* Terminators are execution steps too. Counting only instructions lets
       an empty cyclic block evade the limit forever. */
    if (++steps > context->step_limit) {
      context->result->steps = steps;
      (void)interp_stop(context, QL_IR_INTERP_OUTCOME_STEP_LIMIT,
                        QL_IR_INTERP_UB_NONE, current,
                        QL_IR_INVALID_INSTRUCTION_ID);
      return QL_STATUS_OK;
    }
    context->result->steps = steps;
    context->result->events = context->events;
    switch (block.terminator.kind) {
    case QL_IR_TERMINATOR_RETURN:
      if (!interp_observe(context, block.terminator.return_value, current) ||
          !interp_observe(context, block.terminator.memory, current) ||
          !interp_observe(context, block.terminator.event_trace, current)) {
        return QL_STATUS_OK;
      }
      context->result->outcome = QL_IR_INTERP_OUTCOME_RETURN;
      context->result->block = current;
      context->final_memory =
          block.terminator.memory != QL_IR_INVALID_VALUE_ID
              ? context->values[block.terminator.memory].store
              : context->newest_memory;
      if (block.terminator.return_value != QL_IR_INVALID_VALUE_ID) {
        interp_record_value(context, block.terminator.return_value);
      }
      return QL_STATUS_OK;
    case QL_IR_TERMINATOR_BRANCH:
      previous = current;
      current = block.terminator.target;
      break;
    case QL_IR_TERMINATOR_COND_BRANCH:
      if (!interp_observe(context, block.terminator.condition, current)) {
        return QL_STATUS_OK;
      }
      previous = current;
      current = bits_is_zero(&context->values[block.terminator.condition].bits)
                    ? block.terminator.false_target
                    : block.terminator.target;
      break;
    case QL_IR_TERMINATOR_TRAP:
      context->result->outcome = QL_IR_INTERP_OUTCOME_TRAP;
      context->result->block = current;
      return QL_STATUS_OK;
    case QL_IR_TERMINATOR_UNDEFINED_BEHAVIOR:
      (void)interp_stop(context, QL_IR_INTERP_OUTCOME_UNDEFINED_BEHAVIOR,
                        QL_IR_INTERP_UB_TERMINATOR, current,
                        QL_IR_INVALID_INSTRUCTION_ID);
      return QL_STATUS_OK;
    case QL_IR_TERMINATOR_TERMINATE:
      if (!interp_observe(context, block.terminator.return_value, current)) {
        return QL_STATUS_OK;
      }
      context->result->outcome = QL_IR_INTERP_OUTCOME_TERMINATE;
      context->result->block = current;
      if (block.terminator.return_value != QL_IR_INVALID_VALUE_ID) {
        interp_record_value(context, block.terminator.return_value);
      }
      return QL_STATUS_OK;
    case QL_IR_TERMINATOR_DIVERGE:
      context->result->outcome = QL_IR_INTERP_OUTCOME_DIVERGE;
      context->result->block = current;
      return QL_STATUS_OK;
    default:
      (void)interp_stop(context, QL_IR_INTERP_OUTCOME_UNSUPPORTED,
                        QL_IR_INTERP_UB_NONE, current,
                        QL_IR_INVALID_INSTRUCTION_ID);
      return QL_STATUS_OK;
    }
  }
}

static ql_status interp_prepare(interp_context *context,
                                const ql_ir_interp_input_v1 *inputs,
                                size_t input_count, int *modelled,
                                ql_error *error) {
  size_t index;

  *modelled = 1;
  for (index = 0u; index < context->view.value_count; ++index) {
    ql_ir_value_view_v1 view;
    ql_ir_type_view_v1 type;
    interp_value *value = &context->values[index];

    memset(&view, 0, sizeof(view));
    view.struct_size = sizeof(view);
    if (ql_ir_value_at(context->ir, index, &view, error) != QL_STATUS_OK) {
      return QL_STATUS_INVALID_ARGUMENT;
    }
    memset(&type, 0, sizeof(type));
    type.struct_size = sizeof(type);
    if (ql_ir_type_at(context->ir, view.type, &type, error) != QL_STATUS_OK) {
      return QL_STATUS_INVALID_ARGUMENT;
    }
    value->type = view.type;
    value->kind = type.kind;
    if (type.kind == QL_IR_TYPE_BOOL) {
      value->width = 1u;
    } else if ((type.kind == QL_IR_TYPE_BIT_VECTOR ||
                type.kind == QL_IR_TYPE_POINTER) &&
               type.bit_width <= QL_IR_INTERP_MAX_BIT_WIDTH) {
      value->width = type.bit_width;
    } else if (type.kind == QL_IR_TYPE_MEMORY) {
      /* A memory value carries a version of the object images rather
         than bits. The initial version is the images themselves. */
      value->width = 0u;
      value->store = NULL;
    } else if (type.kind == QL_IR_TYPE_EVENT_TRACE) {
      /* A trace is threaded and observed, never inspected: what the
         calls were is what the callee specification saw. */
      value->width = 0u;
    } else {
      *modelled = 0;
      context->result->outcome = QL_IR_INTERP_OUTCOME_UNSUPPORTED;
      return QL_STATUS_OK;
    }
    if (view.definition_kind == QL_IR_VALUE_CONSTANT) {
      if (!interp_bits_from_bytes(view.constant_data, view.constant_size,
                                  value->width, &value->bits)) {
        ql_error_set(error, QL_STATUS_INVALID_ARGUMENT,
                     "IR constant %zu is not canonically encoded", index);
        return QL_STATUS_INVALID_ARGUMENT;
      }
      value->defined = 1u;
      value->bound = 1u;
    }
  }
  for (index = 0u; index < input_count; ++index) {
    const ql_ir_interp_input_v1 *input = &inputs[index];
    ql_ir_value_view_v1 view;
    interp_value *value;

    if (input->struct_size != 0u && input->struct_size < sizeof(*input)) {
      ql_error_set(error, QL_STATUS_ABI_MISMATCH,
                   "interpreter input structure is too small");
      return QL_STATUS_ABI_MISMATCH;
    }
    if (input->value >= context->view.value_count) {
      ql_error_set(error, QL_STATUS_INVALID_ARGUMENT,
                   "interpreter input %zu names a missing value", index);
      return QL_STATUS_INVALID_ARGUMENT;
    }
    memset(&view, 0, sizeof(view));
    view.struct_size = sizeof(view);
    if (ql_ir_value_at(context->ir, input->value, &view, error) !=
        QL_STATUS_OK) {
      return QL_STATUS_INVALID_ARGUMENT;
    }
    if (view.definition_kind != QL_IR_VALUE_PARAMETER) {
      ql_error_set(error, QL_STATUS_INVALID_ARGUMENT,
                   "interpreter input %zu does not name a parameter", index);
      return QL_STATUS_INVALID_ARGUMENT;
    }
    value = &context->values[input->value];
    if (value->bound != 0u) {
      ql_error_set(error, QL_STATUS_ALREADY_EXISTS,
                   "interpreter input %zu binds a parameter twice", index);
      return QL_STATUS_ALREADY_EXISTS;
    }
    if (value->kind == QL_IR_TYPE_MEMORY ||
        value->kind == QL_IR_TYPE_EVENT_TRACE) {
      /* The object table already supplies the initial image, so a
         memory parameter is bound by naming it and nothing else. */
      if (input->size != 0u) {
        ql_error_set(error, QL_STATUS_INVALID_ARGUMENT,
                     "interpreter input %zu binds a memory or "
                     "event-trace parameter and must carry no bytes",
                     index);
        return QL_STATUS_INVALID_ARGUMENT;
      }
    } else if (!interp_bits_from_bytes(input->data, input->size, value->width,
                                       &value->bits)) {
      ql_error_set(error, QL_STATUS_INVALID_ARGUMENT,
                   "interpreter input %zu does not match its "
                   "parameter's type",
                   index);
      return QL_STATUS_INVALID_ARGUMENT;
    }
    value->defined = 1u;
    value->bound = 1u;
  }
  for (index = 0u; index < context->view.value_count; ++index) {
    ql_ir_value_view_v1 view;
    memset(&view, 0, sizeof(view));
    view.struct_size = sizeof(view);
    if (ql_ir_value_at(context->ir, index, &view, error) != QL_STATUS_OK) {
      return QL_STATUS_INVALID_ARGUMENT;
    }
    if (view.definition_kind == QL_IR_VALUE_PARAMETER &&
        context->values[index].bound == 0u) {
      ql_error_set(error, QL_STATUS_INVALID_ARGUMENT,
                   "parameter %zu has no interpreter input", index);
      return QL_STATUS_INVALID_ARGUMENT;
    }
  }
  return QL_STATUS_OK;
}

ql_status QL_CALL ql_ir_interp_run(const ql_allocator *allocator,
                                   const ql_ir *ir,
                                   const ql_ir_interp_input_v1 *inputs,
                                   size_t input_count,
                                   const ql_ir_interp_options_v1 *options,
                                   ql_ir_interp_result_v1 *result,
                                   ql_error *error) {
  const ql_allocator *selected = allocator;
  interp_context context;
  size_t slots;
  int modelled = 1;
  ql_status status;

  if (ir == NULL || result == NULL || (inputs == NULL && input_count > 0u)) {
    ql_error_set(error, QL_STATUS_INVALID_ARGUMENT,
                 "IR module and interpreter result are required");
    return QL_STATUS_INVALID_ARGUMENT;
  }
  if (result->struct_size != 0u && result->struct_size < sizeof(*result)) {
    ql_error_set(error, QL_STATUS_ABI_MISMATCH,
                 "interpreter result structure is too small");
    return QL_STATUS_ABI_MISMATCH;
  }
  if (options != NULL && options->struct_size != 0u &&
      options->struct_size < sizeof(*options)) {
    ql_error_set(error, QL_STATUS_ABI_MISMATCH,
                 "interpreter option structure is too small");
    return QL_STATUS_ABI_MISMATCH;
  }
  memset(result, 0, sizeof(*result));
  result->struct_size = sizeof(*result);
  result->schema_version = QL_IR_INTERP_SCHEMA_VERSION;
  result->outcome = QL_IR_INTERP_OUTCOME_UNSUPPORTED;
  result->block = QL_IR_INVALID_BLOCK_ID;
  result->instruction = QL_IR_INVALID_INSTRUCTION_ID;
  result->value_type = QL_IR_INVALID_TYPE_ID;
  if (selected == NULL) {
    selected = ql_default_allocator();
  }
  if (!ql_allocator_is_valid(selected)) {
    ql_error_set(error, QL_STATUS_INVALID_ARGUMENT, "invalid allocator");
    return QL_STATUS_INVALID_ARGUMENT;
  }

  memset(&context, 0, sizeof(context));
  context.allocator = *selected;
  context.ir = ir;
  context.result = result;
  context.step_limit = options != NULL && options->step_limit != 0u
                           ? options->step_limit
                           : QL_IR_INTERP_DEFAULT_STEP_LIMIT;
  if (options != NULL) {
    context.objects = options->objects;
    context.object_count = options->object_count;
    context.callees = options->callees;
  }
  status = interp_validate_objects(&context, error);
  if (status != QL_STATUS_OK) {
    return status;
  }
  context.view.struct_size = sizeof(context.view);
  status = ql_ir_get_view(ir, &context.view, error);
  if (status != QL_STATUS_OK) {
    return status;
  }
  slots = context.view.value_count == 0u ? 1u : context.view.value_count;
  context.values = (interp_value *)selected->allocate(
      selected->user_data, slots * sizeof(interp_value));
  context.phi_snapshot = (interp_value *)selected->allocate(
      selected->user_data, slots * sizeof(interp_value));
  if (context.values == NULL || context.phi_snapshot == NULL) {
    selected->deallocate(selected->user_data, context.phi_snapshot);
    selected->deallocate(selected->user_data, context.values);
    ql_error_set(error, QL_STATUS_OUT_OF_MEMORY, NULL);
    return QL_STATUS_OUT_OF_MEMORY;
  }
  memset(context.values, 0, slots * sizeof(interp_value));
  memset(context.phi_snapshot, 0, slots * sizeof(interp_value));

  status = interp_prepare(&context, inputs, input_count, &modelled, error);
  if (status == QL_STATUS_OK && modelled) {
    status = interp_execute(&context, error);
  }
  if (status == QL_STATUS_OK) {
    interp_write_final_images(&context, context.final_memory);
  }
  result->events = context.events;
  interp_release_stores(&context);
  selected->deallocate(selected->user_data, context.phi_snapshot);
  selected->deallocate(selected->user_data, context.values);
  if (status == QL_STATUS_OK) {
    ql_error_clear(error);
  }
  return status;
}
