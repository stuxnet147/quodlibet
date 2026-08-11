#include "loop_proof.h"

#include "quodlibet/ir_interp.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include <uv.h>

#define QL_LOOP_MEMORY_INDEX_WIDTH 64u
#define QL_LOOP_MEMORY_ELEMENT_WIDTH 8u
#define QL_LOOP_EVENT_TRACE_WIDTH 64u

typedef struct loop_buffer {
  ql_allocator allocator;
  char *data;
  size_t size;
  size_t capacity;
} loop_buffer;

typedef struct loop_pair {
  size_t left;
  size_t right;
} loop_pair;

typedef struct loop_scan {
  uint32_t has_assume;
  uint32_t has_ub;
  uint32_t has_call;
  uint32_t has_memory;
  uint32_t has_event_trace;
  uint32_t has_pointer;
  uint32_t has_unsupported_type;
  uint64_t effects;
} loop_scan;

struct ql_loop_proof_query {
  ql_allocator allocator;
  ql_loop_proof_query_view_v1 view;
  ql_loop_relation_candidate_v1 *candidates;
  size_t candidate_capacity;
  ql_artifact *prefix;
  ql_artifact *induction;
  ql_artifact *reflexivity;
  ql_artifact *summary;
  ql_artifact *domain;
  uint32_t promotion_gate_satisfied;
};

typedef struct concrete_witness_input {
  ql_ir_interp_input_v1 input;
  uint8_t bytes[QL_IR_INTERP_VALUE_CAPACITY];
  uint32_t fixed;
} concrete_witness_input;

typedef struct concrete_witness_object {
  ql_ir_interp_object_v1 object;
  ql_ir_value_id base_value;
  ql_ir_value_id size_value;
  const char *name;
  size_t name_size;
  uint8_t *initial;
} concrete_witness_object;

static const ql_allocator *select_allocator(const ql_allocator *allocator) {
  return allocator == NULL ? ql_default_allocator() : allocator;
}

static uint64_t now_ns(void) { return uv_hrtime(); }

static uint64_t elapsed_ns(uint64_t started) {
  const uint64_t ended = now_ns();
  return ended >= started ? ended - started : UINT64_C(0);
}

static void buffer_init(loop_buffer *buffer, const ql_allocator *allocator) {
  memset(buffer, 0, sizeof(*buffer));
  buffer->allocator = *allocator;
}

static void buffer_dispose(loop_buffer *buffer) {
  if (buffer == NULL) {
    return;
  }
  buffer->allocator.deallocate(buffer->allocator.user_data, buffer->data);
  memset(buffer, 0, sizeof(*buffer));
}

static ql_status buffer_reserve(loop_buffer *buffer, size_t additional,
                                ql_error *error) {
  size_t required;
  size_t capacity;
  char *grown;

  if (additional > SIZE_MAX - buffer->size - 1u) {
    ql_error_set(error, QL_STATUS_OUT_OF_MEMORY,
                 "loop-proof SMT text is too large");
    return QL_STATUS_OUT_OF_MEMORY;
  }
  required = buffer->size + additional + 1u;
  if (required <= buffer->capacity) {
    return QL_STATUS_OK;
  }
  capacity = buffer->capacity == 0u ? 1024u : buffer->capacity;
  while (capacity < required) {
    if (capacity > SIZE_MAX / 2u) {
      capacity = required;
      break;
    }
    capacity *= 2u;
  }
  grown = buffer->allocator.reallocate(buffer->allocator.user_data,
                                       buffer->data, capacity);
  if (grown == NULL) {
    ql_error_set(error, QL_STATUS_OUT_OF_MEMORY, NULL);
    return QL_STATUS_OUT_OF_MEMORY;
  }
  buffer->data = grown;
  buffer->capacity = capacity;
  return QL_STATUS_OK;
}

static ql_status buffer_add_n(loop_buffer *buffer, const char *text,
                              size_t size, ql_error *error) {
  ql_status status;

  if (text == NULL && size != 0u) {
    ql_error_set(error, QL_STATUS_INVALID_ARGUMENT,
                 "null loop-proof text has a nonzero size");
    return QL_STATUS_INVALID_ARGUMENT;
  }
  status = buffer_reserve(buffer, size, error);
  if (status != QL_STATUS_OK) {
    return status;
  }
  if (size != 0u) {
    memcpy(buffer->data + buffer->size, text, size);
  }
  buffer->size += size;
  buffer->data[buffer->size] = '\0';
  return QL_STATUS_OK;
}

static ql_status buffer_add(loop_buffer *buffer, const char *text,
                            ql_error *error) {
  return buffer_add_n(buffer, text, strlen(text), error);
}

static ql_status buffer_addf(loop_buffer *buffer, ql_error *error,
                             const char *format, ...) {
  va_list arguments;
  va_list copy;
  int needed;
  ql_status status;

  va_start(arguments, format);
  va_copy(copy, arguments);
  needed = vsnprintf(NULL, 0u, format, copy);
  va_end(copy);
  if (needed < 0) {
    va_end(arguments);
    ql_error_set(error, QL_STATUS_INTERNAL_ERROR,
                 "could not format loop-proof SMT text");
    return QL_STATUS_INTERNAL_ERROR;
  }
  status = buffer_reserve(buffer, (size_t)needed, error);
  if (status == QL_STATUS_OK) {
    const int written =
        vsnprintf(buffer->data + buffer->size, buffer->capacity - buffer->size,
                  format, arguments);
    if (written != needed) {
      ql_error_set(error, QL_STATUS_INTERNAL_ERROR,
                   "loop-proof SMT formatting changed size");
      status = QL_STATUS_INTERNAL_ERROR;
    } else {
      buffer->size += (size_t)written;
    }
  }
  va_end(arguments);
  return status;
}

static int bytes_equal(const void *left, size_t left_size, const void *right,
                       size_t right_size) {
  if (left_size != right_size) {
    return 0;
  }
  if (left_size == 0u) {
    return 1;
  }
  return left != NULL && right != NULL && memcmp(left, right, left_size) == 0;
}

static void digest_buffer(const loop_buffer *buffer, ql_digest *digest) {
  static const char empty[] = "";
  ql_digest_data(buffer->data != NULL ? buffer->data : empty, buffer->size,
                 digest);
}

static ql_status digest_bound_obligation(const ql_allocator *allocator,
                                         const ql_digest *canonical_digest,
                                         const loop_buffer *prefix,
                                         const char *tag,
                                         const loop_buffer *obligation,
                                         ql_digest *digest, ql_error *error) {
  loop_buffer bound;
  char canonical_hex[QL_DIGEST_HEX_SIZE];
  ql_status status;

  buffer_init(&bound, allocator);
  ql_digest_hex(canonical_digest, canonical_hex);
  status = buffer_addf(&bound, error, "canonical=%s\nobligation=%s\n",
                       canonical_hex, tag);
  if (status == QL_STATUS_OK) {
    status = buffer_add_n(&bound, prefix->data, prefix->size, error);
  }
  if (status == QL_STATUS_OK) {
    status = buffer_add(&bound, "\nbody=", error);
  }
  if (status == QL_STATUS_OK) {
    status = buffer_add_n(&bound, obligation->data, obligation->size, error);
  }
  if (status == QL_STATUS_OK) {
    digest_buffer(&bound, digest);
  }
  buffer_dispose(&bound);
  return status;
}

static ql_status artifact_digest(const ql_artifact *artifact, ql_digest *digest,
                                 ql_error *error) {
  ql_artifact_view view;
  ql_status status;

  memset(&view, 0, sizeof(view));
  view.struct_size = sizeof(view);
  status = ql_artifact_get_view(artifact, &view, error);
  if (status == QL_STATUS_OK) {
    *digest = view.digest;
  }
  return status;
}

static void copy_diagnostic(char output[QL_ERROR_MESSAGE_CAPACITY],
                            const char *message) {
  if (message == NULL) {
    output[0] = '\0';
    return;
  }
  (void)snprintf(output, QL_ERROR_MESSAGE_CAPACITY, "%s", message);
}

static ql_smt_product_answer answer_from_solver(ql_solver_check_kind answer) {
  switch (answer) {
  case QL_SOLVER_CHECK_SAT:
    return QL_SMT_PRODUCT_ANSWER_SAT;
  case QL_SOLVER_CHECK_UNSAT:
    return QL_SMT_PRODUCT_ANSWER_UNSAT;
  case QL_SOLVER_CHECK_UNKNOWN:
    return QL_SMT_PRODUCT_ANSWER_UNKNOWN;
  default:
    return QL_SMT_PRODUCT_ANSWER_NOT_QUERIED;
  }
}

static ql_status get_ir_view(const ql_ir *ir, ql_ir_view_v1 *view,
                             ql_error *error) {
  memset(view, 0, sizeof(*view));
  view->struct_size = sizeof(*view);
  return ql_ir_get_view(ir, view, error);
}

static ql_status get_type_view(const ql_ir *ir, size_t index,
                               ql_ir_type_view_v1 *view, ql_error *error) {
  memset(view, 0, sizeof(*view));
  view->struct_size = sizeof(*view);
  return ql_ir_type_at(ir, index, view, error);
}

static ql_status get_value_view(const ql_ir *ir, size_t index,
                                ql_ir_value_view_v1 *view, ql_error *error) {
  memset(view, 0, sizeof(*view));
  view->struct_size = sizeof(*view);
  return ql_ir_value_at(ir, index, view, error);
}

static ql_status get_instruction_view(const ql_ir *ir, size_t index,
                                      ql_ir_instruction_view_v1 *view,
                                      ql_error *error) {
  memset(view, 0, sizeof(*view));
  view->struct_size = sizeof(*view);
  return ql_ir_instruction_at(ir, index, view, error);
}

static ql_status get_block_view(const ql_ir *ir, size_t index,
                                ql_ir_block_view_v1 *view, ql_error *error) {
  memset(view, 0, sizeof(*view));
  view->struct_size = sizeof(*view);
  return ql_ir_block_at(ir, index, view, error);
}

static int type_views_equal(const ql_ir_type_view_v1 *left,
                            const ql_ir_type_view_v1 *right) {
  return left->id == right->id && left->kind == right->kind &&
         left->float_format == right->float_format &&
         left->bit_width == right->bit_width &&
         left->address_space == right->address_space &&
         left->element_type == right->element_type &&
         left->element_count == right->element_count;
}

static int value_views_equal(const ql_ir_value_view_v1 *left,
                             const ql_ir_value_view_v1 *right) {
  return left->id == right->id && left->type == right->type &&
         left->definition_kind == right->definition_kind &&
         left->result_index == right->result_index &&
         left->instruction == right->instruction &&
         bytes_equal(left->constant_data, left->constant_size,
                     right->constant_data, right->constant_size);
}

static int instruction_views_equal(const ql_ir_instruction_view_v1 *left,
                                   const ql_ir_instruction_view_v1 *right) {
  return left->id == right->id && left->block == right->block &&
         left->opcode == right->opcode && left->flags == right->flags &&
         left->effects == right->effects &&
         bytes_equal(left->operands,
                     left->operand_count * sizeof(*left->operands),
                     right->operands,
                     right->operand_count * sizeof(*right->operands)) &&
         bytes_equal(left->block_operands,
                     left->block_operand_count * sizeof(*left->block_operands),
                     right->block_operands,
                     right->block_operand_count *
                         sizeof(*right->block_operands)) &&
         bytes_equal(left->results, left->result_count * sizeof(*left->results),
                     right->results,
                     right->result_count * sizeof(*right->results)) &&
         left->immediate == right->immediate &&
         bytes_equal(left->symbol, left->symbol_size, right->symbol,
                     right->symbol_size) &&
         bytes_equal(left->image, left->image_size, right->image,
                     right->image_size);
}

static int terminators_equal(const ql_ir_terminator_definition_v1 *left,
                             const ql_ir_terminator_definition_v1 *right) {
  return left->kind == right->kind && left->condition == right->condition &&
         left->return_value == right->return_value &&
         left->memory == right->memory &&
         left->event_trace == right->event_trace &&
         left->target == right->target &&
         left->false_target == right->false_target && left->code == right->code;
}

static int block_views_equal(const ql_ir_block_view_v1 *left,
                             const ql_ir_block_view_v1 *right) {
  return left->id == right->id &&
         bytes_equal(left->instructions,
                     left->instruction_count * sizeof(*left->instructions),
                     right->instructions,
                     right->instruction_count * sizeof(*right->instructions)) &&
         terminators_equal(&left->terminator, &right->terminator);
}

/* This is deliberately stronger than a graph-isomorphism test.  IDs are
   positional in canonical typed IR, while labels and function names carry no
   semantics.  Any semantic table difference refuses the shared-UF path. */
static ql_status ir_structurally_equal(const ql_ir *left_ir,
                                       const ql_ir *right_ir, uint32_t *equal,
                                       ql_error *error) {
  ql_ir_view_v1 left_view;
  ql_ir_view_v1 right_view;
  size_t index;
  ql_status status;

  *equal = 0u;
  status = get_ir_view(left_ir, &left_view, error);
  if (status == QL_STATUS_OK) {
    status = get_ir_view(right_ir, &right_view, error);
  }
  if (status != QL_STATUS_OK) {
    return status;
  }
  if (left_view.schema_version != right_view.schema_version ||
      left_view.cfg_kind != right_view.cfg_kind ||
      left_view.return_type != right_view.return_type ||
      left_view.entry_block != right_view.entry_block ||
      left_view.type_count != right_view.type_count ||
      left_view.value_count != right_view.value_count ||
      left_view.block_count != right_view.block_count ||
      left_view.instruction_count != right_view.instruction_count) {
    return QL_STATUS_OK;
  }
  for (index = 0u; index < left_view.type_count; ++index) {
    ql_ir_type_view_v1 left;
    ql_ir_type_view_v1 right;
    status = get_type_view(left_ir, index, &left, error);
    if (status == QL_STATUS_OK) {
      status = get_type_view(right_ir, index, &right, error);
    }
    if (status != QL_STATUS_OK || !type_views_equal(&left, &right)) {
      return status;
    }
  }
  for (index = 0u; index < left_view.value_count; ++index) {
    ql_ir_value_view_v1 left;
    ql_ir_value_view_v1 right;
    status = get_value_view(left_ir, index, &left, error);
    if (status == QL_STATUS_OK) {
      status = get_value_view(right_ir, index, &right, error);
    }
    if (status != QL_STATUS_OK || !value_views_equal(&left, &right)) {
      return status;
    }
  }
  for (index = 0u; index < left_view.instruction_count; ++index) {
    ql_ir_instruction_view_v1 left;
    ql_ir_instruction_view_v1 right;
    status = get_instruction_view(left_ir, index, &left, error);
    if (status == QL_STATUS_OK) {
      status = get_instruction_view(right_ir, index, &right, error);
    }
    if (status != QL_STATUS_OK || !instruction_views_equal(&left, &right)) {
      return status;
    }
  }
  for (index = 0u; index < left_view.block_count; ++index) {
    ql_ir_block_view_v1 left;
    ql_ir_block_view_v1 right;
    status = get_block_view(left_ir, index, &left, error);
    if (status == QL_STATUS_OK) {
      status = get_block_view(right_ir, index, &right, error);
    }
    if (status != QL_STATUS_OK || !block_views_equal(&left, &right)) {
      return status;
    }
  }
  *equal = 1u;
  return QL_STATUS_OK;
}

static int name_has_suffix(const char *name, const char *suffix,
                           size_t *prefix_size) {
  size_t name_size;
  size_t suffix_size;

  if (name == NULL || suffix == NULL) {
    return 0;
  }
  name_size = strlen(name);
  suffix_size = strlen(suffix);
  if (name_size < suffix_size ||
      memcmp(name + name_size - suffix_size, suffix, suffix_size) != 0) {
    return 0;
  }
  if (prefix_size != NULL) {
    *prefix_size = name_size - suffix_size;
  }
  return 1;
}

static ql_status defining_instruction_view(
    const ql_ir *ir, ql_ir_value_id value,
    ql_ir_instruction_view_v1 *instruction, uint32_t *found,
    ql_error *error) {
  ql_ir_value_view_v1 value_view;
  ql_status status;

  *found = 0u;
  status = get_value_view(ir, value, &value_view, error);
  if (status != QL_STATUS_OK ||
      value_view.definition_kind != QL_IR_VALUE_INSTRUCTION_RESULT) {
    return status;
  }
  status = get_instruction_view(ir, value_view.instruction, instruction, error);
  if (status == QL_STATUS_OK) {
    *found = 1u;
  }
  return status;
}

static ql_status unwrap_concrete_parameter(const ql_ir *ir,
                                           ql_ir_value_id value,
                                           ql_ir_value_id *parameter,
                                           ql_error *error) {
  size_t depth;

  *parameter = QL_IR_INVALID_VALUE_ID;
  for (depth = 0u; depth < 32u; ++depth) {
    ql_ir_value_view_v1 value_view;
    ql_ir_instruction_view_v1 instruction;
    uint32_t found;
    ql_status status = get_value_view(ir, value, &value_view, error);
    if (status != QL_STATUS_OK) {
      return status;
    }
    if (value_view.definition_kind == QL_IR_VALUE_PARAMETER) {
      *parameter = value;
      return QL_STATUS_OK;
    }
    status = defining_instruction_view(ir, value, &instruction, &found, error);
    if (status != QL_STATUS_OK || found == 0u ||
        instruction.operand_count != 1u) {
      return status;
    }
    switch (instruction.opcode) {
    case QL_IR_OPCODE_IDENTITY:
    case QL_IR_OPCODE_ZEXT:
    case QL_IR_OPCODE_SEXT:
    case QL_IR_OPCODE_TRUNC:
    case QL_IR_OPCODE_BITCAST:
    case QL_IR_OPCODE_PTR_TO_BV:
    case QL_IR_OPCODE_BV_TO_PTR:
      value = instruction.operands[0];
      break;
    default:
      return QL_STATUS_OK;
    }
  }
  return QL_STATUS_OK;
}

static ql_status concrete_constant_u64(const ql_ir *ir,
                                       ql_ir_value_id value, uint64_t *output,
                                       uint32_t *found, ql_error *error) {
  size_t depth;

  *found = 0u;
  *output = 0u;
  for (depth = 0u; depth < 32u; ++depth) {
    ql_ir_value_view_v1 value_view;
    ql_ir_instruction_view_v1 instruction;
    uint32_t has_instruction;
    size_t byte;
    ql_status status = get_value_view(ir, value, &value_view, error);
    if (status != QL_STATUS_OK) {
      return status;
    }
    if (value_view.definition_kind == QL_IR_VALUE_CONSTANT) {
      if (value_view.constant_data == NULL ||
          value_view.constant_size > sizeof(*output)) {
        return QL_STATUS_OK;
      }
      for (byte = 0u; byte < value_view.constant_size; ++byte) {
        *output |= (uint64_t)((const uint8_t *)value_view.constant_data)[byte]
                   << (byte * 8u);
      }
      *found = 1u;
      return QL_STATUS_OK;
    }
    status = defining_instruction_view(ir, value, &instruction,
                                       &has_instruction, error);
    if (status != QL_STATUS_OK || has_instruction == 0u ||
        instruction.operand_count != 1u) {
      return status;
    }
    switch (instruction.opcode) {
    case QL_IR_OPCODE_IDENTITY:
    case QL_IR_OPCODE_ZEXT:
    case QL_IR_OPCODE_SEXT:
    case QL_IR_OPCODE_TRUNC:
    case QL_IR_OPCODE_BITCAST:
      value = instruction.operands[0];
      break;
    default:
      return QL_STATUS_OK;
    }
  }
  return QL_STATUS_OK;
}

static int QL_CALL patterned_callee(
    void *user_data, const char *symbol,
    const ql_ir_interp_argument_v1 *arguments, size_t argument_count,
    void *result, size_t result_size) {
  const uint64_t word = *(const uint64_t *)user_data;
  size_t byte;
  (void)symbol;
  (void)arguments;
  (void)argument_count;
  if (result == NULL && result_size != 0u) {
    return 0;
  }
  for (byte = 0u; byte < result_size; ++byte) {
    ((uint8_t *)result)[byte] =
        (uint8_t)((word >> ((byte % sizeof(word)) * 8u)) & UINT64_C(0xff));
  }
  return 1;
}

static concrete_witness_object *find_witness_object_by_value(
    concrete_witness_object *objects, size_t object_count,
    ql_ir_value_id value, int select_base) {
  size_t index;
  for (index = 0u; index < object_count; ++index) {
    ql_ir_value_id candidate =
        select_base != 0 ? objects[index].base_value : objects[index].size_value;
    if (candidate == value) {
      return &objects[index];
    }
  }
  return NULL;
}

static concrete_witness_object *find_witness_object_by_name(
    concrete_witness_object *objects, size_t object_count, const char *name) {
  size_t index;
  size_t name_size;

  if (name == NULL) {
    return NULL;
  }
  name_size = strlen(name);
  for (index = 0u; index < object_count; ++index) {
    if (objects[index].name_size == name_size &&
        memcmp(objects[index].name, name, name_size) == 0) {
      return &objects[index];
    }
  }
  return NULL;
}

/* A concrete defined run proves only that the comparison domain is inhabited.
   It is not used as evidence for the universal equivalence claim.  The finite
   witness search tries zero, one, and all-ones scalar inputs, points each
   source pointer at its own valid zero-filled object, supplies disjoint object
   descriptors, and chooses a deterministic pure callee interpretation that
   returns either zero or an existing object base.  A run that hits UB, an
   assumption rejection, unsupported semantics, or the step limit proves
   nothing and is simply not accepted. */
static ql_status build_concrete_domain_witness(
    const ql_allocator *allocator, const ql_ir *ir, uint32_t *witnessed,
    ql_digest *witness_digest,
    char reason[QL_ERROR_MESSAGE_CAPACITY], ql_error *error) {
  static const uint64_t default_object_size = UINT64_C(4096);
  static const uint64_t maximum_object_size = UINT64_C(64) * 1024u * 1024u;
  ql_ir_view_v1 ir_view;
  concrete_witness_input *inputs = NULL;
  ql_ir_interp_input_v1 *interp_inputs = NULL;
  concrete_witness_object *objects = NULL;
  ql_ir_interp_object_v1 *interp_objects = NULL;
  ql_ir_interp_options_v1 options;
  ql_ir_interp_callees_v1 callees;
  ql_ir_interp_result_v1 result;
  loop_buffer evidence;
  size_t parameter_count = 0u;
  size_t object_count = 0u;
  size_t input_index = 0u;
  size_t object_index = 0u;
  size_t value_index;
  size_t instruction_index;
  size_t scalar_pattern_index;
  size_t callee_pattern_index;
  size_t accepted_scalar_pattern = 0u;
  size_t accepted_callee_pattern = 0u;
  uint64_t cursor = QL_IR_INTERP_FIRST_OBJECT_ADDRESS;
  uint64_t total_object_size = 0u;
  uint64_t callee_word = 0u;
  uint32_t defined = 0u;
  ql_status status;

  *witnessed = 0u;
  memset(witness_digest, 0, sizeof(*witness_digest));
  copy_diagnostic(reason, "witness preparation could not model the IR");
  buffer_init(&evidence, allocator);
  status = get_ir_view(ir, &ir_view, error);
  if (status != QL_STATUS_OK) {
    goto cleanup;
  }
  for (value_index = 0u; value_index < ir_view.value_count; ++value_index) {
    ql_ir_value_view_v1 value;
    size_t prefix_size;
    status = get_value_view(ir, value_index, &value, error);
    if (status != QL_STATUS_OK) {
      goto cleanup;
    }
    if (value.definition_kind != QL_IR_VALUE_PARAMETER) {
      continue;
    }
    ++parameter_count;
    if (name_has_suffix(value.name, ".__base", &prefix_size)) {
      ++object_count;
    }
  }
  if (parameter_count != 0u) {
    inputs = allocator->allocate(allocator->user_data,
                                 parameter_count * sizeof(*inputs));
    interp_inputs = allocator->allocate(
        allocator->user_data, parameter_count * sizeof(*interp_inputs));
    if (inputs == NULL || interp_inputs == NULL) {
      status = QL_STATUS_OUT_OF_MEMORY;
      ql_error_set(error, status, NULL);
      goto cleanup;
    }
    memset(inputs, 0, parameter_count * sizeof(*inputs));
    memset(interp_inputs, 0, parameter_count * sizeof(*interp_inputs));
  }
  if (object_count != 0u) {
    objects = allocator->allocate(allocator->user_data,
                                  object_count * sizeof(*objects));
    interp_objects = allocator->allocate(
        allocator->user_data, object_count * sizeof(*interp_objects));
    if (objects == NULL || interp_objects == NULL) {
      status = QL_STATUS_OUT_OF_MEMORY;
      ql_error_set(error, status, NULL);
      goto cleanup;
    }
    memset(objects, 0, object_count * sizeof(*objects));
    memset(interp_objects, 0, object_count * sizeof(*interp_objects));
  }

  for (value_index = 0u; value_index < ir_view.value_count; ++value_index) {
    ql_ir_value_view_v1 value;
    size_t prefix_size;
    status = get_value_view(ir, value_index, &value, error);
    if (status != QL_STATUS_OK) {
      goto cleanup;
    }
    if (value.definition_kind != QL_IR_VALUE_PARAMETER ||
        !name_has_suffix(value.name, ".__base", &prefix_size)) {
      continue;
    }
    objects[object_index].base_value = value.id;
    objects[object_index].size_value = QL_IR_INVALID_VALUE_ID;
    objects[object_index].name = value.name;
    objects[object_index].name_size = prefix_size;
    ql_ir_interp_object_init(&objects[object_index].object);
    objects[object_index].object.size = default_object_size;
    ++object_index;
  }
  for (value_index = 0u; value_index < ir_view.value_count; ++value_index) {
    ql_ir_value_view_v1 value;
    size_t prefix_size;
    status = get_value_view(ir, value_index, &value, error);
    if (status != QL_STATUS_OK) {
      goto cleanup;
    }
    if (value.definition_kind == QL_IR_VALUE_PARAMETER &&
        name_has_suffix(value.name, ".__size", &prefix_size)) {
      for (object_index = 0u; object_index < object_count; ++object_index) {
        if (objects[object_index].name_size == prefix_size &&
            memcmp(objects[object_index].name, value.name, prefix_size) == 0) {
          objects[object_index].size_value = value.id;
          break;
        }
      }
    }
  }
  for (object_index = 0u; object_index < object_count; ++object_index) {
    if (objects[object_index].size_value == QL_IR_INVALID_VALUE_ID) {
      copy_diagnostic(reason, "an object has no matching size parameter");
      status = QL_STATUS_OK;
      goto cleanup;
    }
  }

  /* Recover the fixed-size equalities emitted for globals, literals, and
     stack objects.  Pointer-argument and dynamic objects keep the admissible
     default size. */
  for (instruction_index = 0u;
       instruction_index < ir_view.instruction_count; ++instruction_index) {
    ql_ir_instruction_view_v1 assume;
    ql_ir_instruction_view_v1 predicate;
    uint32_t found;
    size_t side;
    status = get_instruction_view(ir, instruction_index, &assume, error);
    if (status != QL_STATUS_OK) {
      goto cleanup;
    }
    if (assume.opcode != QL_IR_OPCODE_ASSUME || assume.operand_count != 1u) {
      continue;
    }
    status = defining_instruction_view(ir, assume.operands[0], &predicate,
                                       &found, error);
    if (status != QL_STATUS_OK) {
      goto cleanup;
    }
    if (found == 0u || predicate.opcode != QL_IR_OPCODE_EQ ||
        predicate.operand_count != 2u) {
      continue;
    }
    for (side = 0u; side < 2u; ++side) {
      ql_ir_value_id parameter;
      uint64_t constant;
      uint32_t constant_found;
      concrete_witness_object *object;
      status = unwrap_concrete_parameter(ir, predicate.operands[side],
                                         &parameter, error);
      if (status == QL_STATUS_OK) {
        status = concrete_constant_u64(ir, predicate.operands[1u - side],
                                       &constant, &constant_found, error);
      }
      if (status != QL_STATUS_OK) {
        goto cleanup;
      }
      object = find_witness_object_by_value(objects, object_count, parameter,
                                            0);
      if (object != NULL && constant_found != 0u && constant != 0u) {
        object->object.size = constant;
      }
    }
  }
  for (object_index = 0u; object_index < object_count; ++object_index) {
    concrete_witness_object *object = &objects[object_index];
    uint64_t aligned;
    if (object->object.size == 0u ||
        object->object.size > maximum_object_size ||
        object->object.size > maximum_object_size - total_object_size ||
        cursor > UINT64_MAX - UINT64_C(4095)) {
      copy_diagnostic(reason,
                      "an object size exceeds the concrete witness limit");
      status = QL_STATUS_OK;
      goto cleanup;
    }
    aligned = (cursor + UINT64_C(4095)) & ~UINT64_C(4095);
    if (object->object.size > UINT64_MAX - aligned - UINT64_C(4096)) {
      copy_diagnostic(reason,
                      "object addresses overflow the interpreter range");
      status = QL_STATUS_OK;
      goto cleanup;
    }
    object->object.base = aligned;
    total_object_size += object->object.size;
    cursor = aligned + object->object.size + UINT64_C(4096);
    object->initial = allocator->allocate(
        allocator->user_data, (size_t)object->object.size);
    if (object->initial == NULL) {
      status = QL_STATUS_OUT_OF_MEMORY;
      ql_error_set(error, status, NULL);
      goto cleanup;
    }
    memset(object->initial, 0, (size_t)object->object.size);
    object->object.initial = object->initial;
  }

  /* Materialize bytes that the IR states about an object, such as a string
     literal.  The lowering emits these at the object's base. */
  for (instruction_index = 0u;
       instruction_index < ir_view.instruction_count; ++instruction_index) {
    ql_ir_instruction_view_v1 instruction;
    ql_ir_value_id parameter;
    concrete_witness_object *object;
    status = get_instruction_view(ir, instruction_index, &instruction, error);
    if (status != QL_STATUS_OK) {
      goto cleanup;
    }
    if (instruction.opcode != QL_IR_OPCODE_MEMORY_IMAGE ||
        instruction.operand_count < 2u || instruction.image == NULL) {
      continue;
    }
    status = unwrap_concrete_parameter(ir, instruction.operands[1], &parameter,
                                       error);
    if (status != QL_STATUS_OK) {
      goto cleanup;
    }
    object = find_witness_object_by_value(objects, object_count, parameter, 1);
    if (object != NULL && instruction.image_size <= object->object.size) {
      memcpy(object->initial, instruction.image, instruction.image_size);
    }
  }

  for (value_index = 0u; value_index < ir_view.value_count; ++value_index) {
    ql_ir_value_view_v1 value;
    ql_ir_type_view_v1 type;
    concrete_witness_object *object = NULL;
    size_t byte_size;
    status = get_value_view(ir, value_index, &value, error);
    if (status != QL_STATUS_OK) {
      goto cleanup;
    }
    if (value.definition_kind != QL_IR_VALUE_PARAMETER) {
      continue;
    }
    status = get_type_view(ir, value.type, &type, error);
    if (status != QL_STATUS_OK) {
      goto cleanup;
    }
    ql_ir_interp_input_init(&inputs[input_index].input);
    inputs[input_index].input.value = value.id;
    if (type.kind == QL_IR_TYPE_MEMORY ||
        type.kind == QL_IR_TYPE_EVENT_TRACE) {
      ++input_index;
      continue;
    }
    if (type.kind == QL_IR_TYPE_BOOL) {
      byte_size = 1u;
    } else if (type.bit_width == 0u ||
               type.bit_width > QL_IR_INTERP_MAX_BIT_WIDTH) {
      copy_diagnostic(reason,
                      "a parameter type is outside the interpreter witness "
                      "width");
      status = QL_STATUS_OK;
      goto cleanup;
    } else {
      byte_size = (type.bit_width + 7u) / 8u;
    }
    memset(inputs[input_index].bytes, 0, sizeof(inputs[input_index].bytes));
    object = find_witness_object_by_value(objects, object_count, value.id, 1);
    if (object == NULL) {
      object = find_witness_object_by_value(objects, object_count, value.id, 0);
    }
    if (object == NULL && type.kind == QL_IR_TYPE_POINTER) {
      object = find_witness_object_by_name(objects, object_count, value.name);
    }
    if (object != NULL) {
      const uint64_t word =
          value.id == object->size_value ? object->object.size
                                         : object->object.base;
      size_t byte;
      for (byte = 0u; byte < byte_size && byte < sizeof(word); ++byte) {
        inputs[input_index].bytes[byte] =
            (uint8_t)((word >> (byte * 8u)) & UINT64_C(0xff));
      }
      inputs[input_index].fixed = 1u;
    } else if (type.kind == QL_IR_TYPE_POINTER) {
      /* A pointer without an associated authority object stays null. */
      inputs[input_index].fixed = 1u;
    }
    inputs[input_index].input.data = inputs[input_index].bytes;
    inputs[input_index].input.size = byte_size;
    ++input_index;
  }
  for (object_index = 0u; object_index < object_count; ++object_index) {
    interp_objects[object_index] = objects[object_index].object;
  }
  for (input_index = 0u; input_index < parameter_count; ++input_index) {
    interp_inputs[input_index] = inputs[input_index].input;
  }

  ql_ir_interp_options_init(&options);
  memset(&callees, 0, sizeof(callees));
  callees.struct_size = sizeof(callees);
  callees.invoke = patterned_callee;
  callees.user_data = &callee_word;
  options.step_limit = UINT64_C(100000);
  options.objects = interp_objects;
  options.object_count = object_count;
  options.callees = &callees;
  for (callee_pattern_index = 0u;
       callee_pattern_index < (object_count == 0u ? 1u : 2u) && defined == 0u;
       ++callee_pattern_index) {
    callee_word = callee_pattern_index == 0u
                      ? UINT64_C(0)
                      : objects[0].object.base;
    for (scalar_pattern_index = 0u; scalar_pattern_index < 3u;
         ++scalar_pattern_index) {
      static const uint64_t scalar_patterns[] = {
          UINT64_C(0), UINT64_C(1), UINT64_MAX};
      const uint64_t scalar_word = scalar_patterns[scalar_pattern_index];

      for (input_index = 0u; input_index < parameter_count; ++input_index) {
        size_t byte;
        if (inputs[input_index].fixed != 0u ||
            inputs[input_index].input.data == NULL) {
          continue;
        }
        for (byte = 0u; byte < inputs[input_index].input.size; ++byte) {
          inputs[input_index].bytes[byte] = (uint8_t)(
              (scalar_word >> ((byte % sizeof(scalar_word)) * 8u)) &
              UINT64_C(0xff));
        }
      }
      memset(&result, 0, sizeof(result));
      result.struct_size = sizeof(result);
      status = ql_ir_interp_run(
          allocator, ir, parameter_count == 0u ? NULL : interp_inputs,
          parameter_count, &options, &result, error);
      if (status != QL_STATUS_OK) {
        (void)snprintf(reason, QL_ERROR_MESSAGE_CAPACITY,
                       "the interpreter rejected witness candidate %u/%u: %s",
                       (unsigned)scalar_pattern_index,
                       (unsigned)callee_pattern_index,
                       error != NULL ? error->message : "unknown error");
        ql_error_clear(error);
        status = QL_STATUS_OK;
        continue;
      }
      if (result.outcome == QL_IR_INTERP_OUTCOME_RETURN ||
          result.outcome == QL_IR_INTERP_OUTCOME_TRAP ||
          result.outcome == QL_IR_INTERP_OUTCOME_TERMINATE ||
          result.outcome == QL_IR_INTERP_OUTCOME_DIVERGE) {
        defined = 1u;
        accepted_scalar_pattern = scalar_pattern_index;
        accepted_callee_pattern = callee_pattern_index;
        break;
      }
      (void)snprintf(reason, QL_ERROR_MESSAGE_CAPACITY,
                     "witness candidate %u/%u ended as %s",
                     (unsigned)scalar_pattern_index,
                     (unsigned)callee_pattern_index,
                     ql_ir_interp_outcome_string(result.outcome));
    }
  }
  if (defined == 0u) {
    goto cleanup;
  }

  status = buffer_add(&evidence, "concrete-domain-witness-v1\n", error);
  for (input_index = 0u;
       status == QL_STATUS_OK && input_index < parameter_count; ++input_index) {
    size_t byte;
    status = buffer_addf(&evidence, error, "input=%u:",
                         (unsigned)inputs[input_index].input.value);
    for (byte = 0u;
         status == QL_STATUS_OK && byte < inputs[input_index].input.size;
         ++byte) {
      status = buffer_addf(&evidence, error, "%02x",
                           inputs[input_index].bytes[byte]);
    }
    if (status == QL_STATUS_OK) {
      status = buffer_add(&evidence, "\n", error);
    }
  }
  for (object_index = 0u;
       status == QL_STATUS_OK && object_index < object_count; ++object_index) {
    ql_digest image_digest;
    char image_hex[QL_DIGEST_HEX_SIZE];
    ql_digest_data(objects[object_index].initial,
                   (size_t)objects[object_index].object.size, &image_digest);
    ql_digest_hex(&image_digest, image_hex);
    status = buffer_addf(
        &evidence, error, "object=%u:%llu:%llu:%s\n",
        (unsigned)objects[object_index].base_value,
        (unsigned long long)objects[object_index].object.base,
        (unsigned long long)objects[object_index].object.size, image_hex);
  }
  if (status == QL_STATUS_OK) {
    status = buffer_addf(
        &evidence, error,
        "outcome=%s\nsteps=%llu\nscalar-pattern=%u\ncallee-pattern=%u\n"
        "callee-word=%llu\n",
        ql_ir_interp_outcome_string(result.outcome),
        (unsigned long long)result.steps,
        (unsigned)accepted_scalar_pattern,
        (unsigned)accepted_callee_pattern,
        (unsigned long long)callee_word);
  }
  if (status == QL_STATUS_OK) {
    digest_buffer(&evidence, witness_digest);
    *witnessed = 1u;
    copy_diagnostic(reason, "a concrete defined execution was found");
  }

cleanup:
  if (objects != NULL) {
    for (object_index = 0u; object_index < object_count; ++object_index) {
      allocator->deallocate(allocator->user_data, objects[object_index].initial);
    }
  }
  allocator->deallocate(allocator->user_data, interp_objects);
  allocator->deallocate(allocator->user_data, objects);
  allocator->deallocate(allocator->user_data, interp_inputs);
  allocator->deallocate(allocator->user_data, inputs);
  buffer_dispose(&evidence);
  return status;
}

static ql_status scan_loop(const ql_ir *ir, const ql_loop_view *loop,
                           loop_scan *scan, ql_error *error) {
  size_t block_index;
  ql_status status = QL_STATUS_OK;

  memset(scan, 0, sizeof(*scan));
  scan->has_memory = loop->has_memory_state;
  scan->has_event_trace = loop->has_event_trace_state;
  scan->effects = loop->effects;
  for (block_index = 0u;
       block_index < loop->block_count && status == QL_STATUS_OK;
       ++block_index) {
    ql_ir_block_view_v1 block;
    size_t instruction_index;
    status = get_block_view(ir, loop->blocks[block_index], &block, error);
    if (status != QL_STATUS_OK) {
      break;
    }
    if (block.terminator.kind == QL_IR_TERMINATOR_UNDEFINED_BEHAVIOR) {
      scan->has_ub = 1u;
    }
    for (instruction_index = 0u;
         instruction_index < block.instruction_count && status == QL_STATUS_OK;
         ++instruction_index) {
      ql_ir_instruction_view_v1 instruction;
      status = get_instruction_view(ir, block.instructions[instruction_index],
                                    &instruction, error);
      if (status != QL_STATUS_OK) {
        break;
      }
      scan->effects |= instruction.effects;
      if (instruction.opcode == QL_IR_OPCODE_ASSUME) {
        scan->has_assume = 1u;
      } else if (instruction.opcode == QL_IR_OPCODE_UB_GUARD) {
        scan->has_ub = 1u;
      } else if (instruction.opcode == QL_IR_OPCODE_CALL) {
        scan->has_call = 1u;
      }
    }
  }
  return status;
}

static int phi_sort_supported(const ql_loop_phi_view *phi,
                              uint32_t allow_opaque_state) {
  switch (phi->type_kind) {
  case QL_IR_TYPE_BOOL:
    return 1;
  case QL_IR_TYPE_BIT_VECTOR:
    return phi->bit_width != 0u;
  case QL_IR_TYPE_POINTER:
    return allow_opaque_state != 0u && phi->bit_width != 0u;
  case QL_IR_TYPE_MEMORY:
  case QL_IR_TYPE_EVENT_TRACE:
    return allow_opaque_state != 0u;
  default:
    return 0;
  }
}

static int loop_shape_compatible(const ql_loop_view *left,
                                 const ql_loop_view *right) {
  size_t index;

  if (left->phi_count != right->phi_count ||
      left->guard.position != right->guard.position ||
      left->guard.continue_on_true != right->guard.continue_on_true ||
      left->nesting_depth != right->nesting_depth ||
      left->is_reducible_single_entry != right->is_reducible_single_entry) {
    return 0;
  }
  for (index = 0u; index < left->phi_count; ++index) {
    const ql_loop_phi_view *left_phi = &left->phis[index];
    const ql_loop_phi_view *right_phi = &right->phis[index];
    if (left_phi->type_kind != right_phi->type_kind ||
        left_phi->bit_width != right_phi->bit_width ||
        left_phi->latch_value_count != right_phi->latch_value_count) {
      return 0;
    }
  }
  return 1;
}

static ql_status pair_loops(const ql_allocator *allocator,
                            const ql_loop_analysis *left_analysis,
                            const ql_loop_analysis *right_analysis,
                            loop_pair **output_pairs, size_t *output_count,
                            ql_error *error) {
  const ql_loop_analysis_view *left_view =
      ql_loop_analysis_get_view(left_analysis);
  const ql_loop_analysis_view *right_view =
      ql_loop_analysis_get_view(right_analysis);
  loop_pair *pairs = NULL;
  uint8_t *right_used = NULL;
  size_t left_index;
  size_t pair_count = 0u;

  *output_pairs = NULL;
  *output_count = 0u;
  if (left_view == NULL || right_view == NULL) {
    ql_error_set(error, QL_STATUS_INTERNAL_ERROR,
                 "loop analyzer returned no view");
    return QL_STATUS_INTERNAL_ERROR;
  }
  if (left_view->loop_count != right_view->loop_count) {
    return QL_STATUS_OK;
  }
  if (left_view->loop_count != 0u) {
    if (left_view->loop_count > SIZE_MAX / sizeof(*pairs)) {
      ql_error_set(error, QL_STATUS_OUT_OF_MEMORY, NULL);
      return QL_STATUS_OUT_OF_MEMORY;
    }
    pairs = allocator->allocate(allocator->user_data,
                                left_view->loop_count * sizeof(*pairs));
    right_used =
        allocator->allocate(allocator->user_data, right_view->loop_count);
    if (pairs == NULL || right_used == NULL) {
      allocator->deallocate(allocator->user_data, pairs);
      allocator->deallocate(allocator->user_data, right_used);
      ql_error_set(error, QL_STATUS_OUT_OF_MEMORY, NULL);
      return QL_STATUS_OUT_OF_MEMORY;
    }
    memset(right_used, 0, right_view->loop_count);
  }
  for (left_index = 0u; left_index < left_view->loop_count; ++left_index) {
    const ql_loop_view *left_loop =
        ql_loop_analysis_loop_at(left_analysis, left_index);
    size_t right_index;
    size_t selected = QL_LOOP_INVALID_INDEX;

    if (left_loop == NULL) {
      allocator->deallocate(allocator->user_data, pairs);
      allocator->deallocate(allocator->user_data, right_used);
      ql_error_set(error, QL_STATUS_INTERNAL_ERROR,
                   "loop analyzer omitted left loop %zu", left_index);
      return QL_STATUS_INTERNAL_ERROR;
    }
    /* Analyzer order is deterministic, but block IDs and labels are never a
       pairing proof.  Prefer the same ordinal only when its canonical shape
       matches, then greedily search the remaining shape-equivalent loops. */
    if (left_index < right_view->loop_count && right_used[left_index] == 0u) {
      const ql_loop_view *right_loop =
          ql_loop_analysis_loop_at(right_analysis, left_index);
      if (right_loop != NULL && loop_shape_compatible(left_loop, right_loop)) {
        selected = left_index;
      }
    }
    for (right_index = 0u; selected == QL_LOOP_INVALID_INDEX &&
                           right_index < right_view->loop_count;
         ++right_index) {
      const ql_loop_view *right_loop;
      if (right_used[right_index] != 0u) {
        continue;
      }
      right_loop = ql_loop_analysis_loop_at(right_analysis, right_index);
      if (right_loop != NULL && loop_shape_compatible(left_loop, right_loop)) {
        selected = right_index;
      }
    }
    if (selected == QL_LOOP_INVALID_INDEX) {
      break;
    }
    pairs[pair_count].left = left_index;
    pairs[pair_count].right = selected;
    right_used[selected] = 1u;
    ++pair_count;
  }
  allocator->deallocate(allocator->user_data, right_used);
  *output_pairs = pairs;
  *output_count = pair_count;
  return QL_STATUS_OK;
}

static uint64_t width_mask(uint32_t width) {
  if (width >= 64u) {
    return UINT64_MAX;
  }
  return (UINT64_C(1) << width) - UINT64_C(1);
}

static uint64_t modular_value(uint64_t value, uint32_t width) {
  return value & width_mask(width);
}

static int recurrence_coefficients(const ql_loop_phi_view *phi, uint64_t *a,
                                   uint64_t *b) {
  const uint32_t width = phi->bit_width;

  if (width == 0u || width > 64u ||
      (phi->type_kind != QL_IR_TYPE_BIT_VECTOR &&
       phi->type_kind != QL_IR_TYPE_POINTER)) {
    return 0;
  }
  switch (phi->recurrence) {
  case QL_LOOP_RECURRENCE_IDENTITY:
    *a = UINT64_C(1);
    *b = UINT64_C(0);
    return 1;
  case QL_LOOP_RECURRENCE_ADD_CONSTANT:
  case QL_LOOP_RECURRENCE_POINTER_STRIDE:
    *a = UINT64_C(1);
    *b = modular_value(phi->step, width);
    return 1;
  case QL_LOOP_RECURRENCE_SUB_CONSTANT:
    *a = UINT64_C(1);
    *b = modular_value(UINT64_C(0) - phi->step, width);
    return 1;
  case QL_LOOP_RECURRENCE_AFFINE_MUL_ADD:
    *a = modular_value(phi->multiplier, width);
    *b = modular_value(phi->offset, width);
    return 1;
  default:
    return 0;
  }
}

static ql_status constant_u64(const ql_ir *ir, ql_ir_value_id value,
                              uint32_t width, uint64_t *output, uint32_t *ok,
                              ql_error *error) {
  ql_ir_value_view_v1 view;
  const uint8_t *bytes;
  size_t index;
  uint64_t result = UINT64_C(0);
  ql_status status;

  *ok = 0u;
  if (width == 0u || width > 64u) {
    return QL_STATUS_OK;
  }
  status = get_value_view(ir, value, &view, error);
  if (status != QL_STATUS_OK) {
    return status;
  }
  if (view.definition_kind != QL_IR_VALUE_CONSTANT ||
      view.constant_size > sizeof(result) || view.constant_data == NULL) {
    return QL_STATUS_OK;
  }
  bytes = (const uint8_t *)view.constant_data;
  for (index = 0u; index < view.constant_size; ++index) {
    result |= (uint64_t)bytes[index] << (index * 8u);
  }
  *output = modular_value(result, width);
  *ok = 1u;
  return QL_STATUS_OK;
}

typedef enum entry_leaf_kind {
  ENTRY_LEAF_UNSUPPORTED = 0,
  ENTRY_LEAF_PARAMETER,
  ENTRY_LEAF_CONSTANT
} entry_leaf_kind;

typedef struct entry_leaf {
  entry_leaf_kind kind;
  size_t parameter_ordinal;
  uint64_t constant;
} entry_leaf;

static ql_status parameter_ordinal(const ql_ir *ir, ql_ir_value_id value,
                                   size_t *ordinal, uint32_t *found,
                                   ql_error *error) {
  ql_ir_view_v1 ir_view;
  size_t index;
  size_t next = 0u;
  ql_status status = get_ir_view(ir, &ir_view, error);

  *found = 0u;
  for (index = 0u; status == QL_STATUS_OK && index < ir_view.value_count;
       ++index) {
    ql_ir_value_view_v1 view;
    status = get_value_view(ir, index, &view, error);
    if (status != QL_STATUS_OK) {
      break;
    }
    if (view.definition_kind != QL_IR_VALUE_PARAMETER) {
      continue;
    }
    if (view.id == value) {
      *ordinal = next;
      *found = 1u;
      break;
    }
    ++next;
  }
  return status;
}

static ql_status entry_leaf_read(const ql_ir *ir, ql_ir_value_id value,
                                 uint32_t width, entry_leaf *leaf,
                                 ql_error *error) {
  ql_ir_value_view_v1 view;
  uint32_t found;
  ql_status status;

  memset(leaf, 0, sizeof(*leaf));
  status = get_value_view(ir, value, &view, error);
  if (status != QL_STATUS_OK) {
    return status;
  }
  if (view.definition_kind == QL_IR_VALUE_PARAMETER) {
    status =
        parameter_ordinal(ir, value, &leaf->parameter_ordinal, &found, error);
    if (status == QL_STATUS_OK && found != 0u) {
      leaf->kind = ENTRY_LEAF_PARAMETER;
    }
    return status;
  }
  if (view.definition_kind == QL_IR_VALUE_CONSTANT) {
    status = constant_u64(ir, value, width, &leaf->constant, &found, error);
    if (status == QL_STATUS_OK && found != 0u) {
      leaf->kind = ENTRY_LEAF_CONSTANT;
    }
  }
  return status;
}

static int entry_leaves_equal(const entry_leaf *left, const entry_leaf *right) {
  if (left->kind == ENTRY_LEAF_PARAMETER &&
      right->kind == ENTRY_LEAF_PARAMETER) {
    return left->parameter_ordinal == right->parameter_ordinal;
  }
  if (left->kind == ENTRY_LEAF_CONSTANT && right->kind == ENTRY_LEAF_CONSTANT) {
    return left->constant == right->constant;
  }
  return 0;
}

static ql_status add_candidate(ql_loop_proof_query *query,
                               const ql_loop_relation_candidate_v1 *candidate,
                               ql_error *error) {
  ql_loop_relation_candidate_v1 *grown;
  size_t capacity;

  if (query->view.candidate_count == query->candidate_capacity) {
    capacity =
        query->candidate_capacity == 0u ? 16u : query->candidate_capacity * 2u;
    if (capacity < query->candidate_capacity ||
        capacity > SIZE_MAX / sizeof(*query->candidates)) {
      ql_error_set(error, QL_STATUS_OUT_OF_MEMORY, NULL);
      return QL_STATUS_OUT_OF_MEMORY;
    }
    grown = query->allocator.reallocate(query->allocator.user_data,
                                        query->candidates,
                                        capacity * sizeof(*query->candidates));
    if (grown == NULL) {
      ql_error_set(error, QL_STATUS_OUT_OF_MEMORY, NULL);
      return QL_STATUS_OUT_OF_MEMORY;
    }
    query->candidates = grown;
    query->candidate_capacity = capacity;
  }
  query->candidates[query->view.candidate_count] = *candidate;
  ++query->view.candidate_count;
  return QL_STATUS_OK;
}

static int affine_relation_is_preserved(uint32_t width, uint64_t left_a,
                                        uint64_t left_b, uint64_t right_a,
                                        uint64_t right_b, uint64_t relation_a,
                                        uint64_t relation_b) {
  const uint64_t mask = width_mask(width);
  const uint64_t state_left = (left_a * relation_a) & mask;
  const uint64_t state_right = (relation_a * right_a) & mask;
  const uint64_t constant_left = (left_a * relation_b + left_b) & mask;
  const uint64_t constant_right = (relation_a * right_b + relation_b) & mask;
  return state_left == state_right && constant_left == constant_right;
}

static uint64_t odd_modular_inverse(uint64_t value, uint32_t width) {
  uint64_t inverse = UINT64_C(1);
  unsigned round;

  /* Newton iteration doubles the number of correct low bits each round. */
  for (round = 0u; round < 6u; ++round) {
    inverse *= UINT64_C(2) - value * inverse;
  }
  return modular_value(inverse, width);
}

static ql_status
generate_phi_candidates(ql_loop_proof_query *query, const ql_ir *left_ir,
                        const ql_ir *right_ir, size_t left_loop_index,
                        size_t right_loop_index, size_t phi_index,
                        const ql_loop_phi_view *left_phi,
                        const ql_loop_phi_view *right_phi, ql_error *error) {
  ql_loop_relation_candidate_v1 candidate;
  entry_leaf left_entry;
  entry_leaf right_entry;
  uint64_t left_a = UINT64_C(0);
  uint64_t left_b = UINT64_C(0);
  uint64_t right_a = UINT64_C(0);
  uint64_t right_b = UINT64_C(0);
  uint32_t selected = 0u;
  ql_status status;

  status = entry_leaf_read(
      left_ir, left_phi->entry_value,
      left_phi->type_kind == QL_IR_TYPE_BOOL ? 1u : left_phi->bit_width,
      &left_entry, error);
  if (status == QL_STATUS_OK) {
    status = entry_leaf_read(
        right_ir, right_phi->entry_value,
        right_phi->type_kind == QL_IR_TYPE_BOOL ? 1u : right_phi->bit_width,
        &right_entry, error);
  }
  if (status != QL_STATUS_OK) {
    return status;
  }
  memset(&candidate, 0, sizeof(candidate));
  candidate.struct_size = sizeof(candidate);
  candidate.kind = QL_LOOP_RELATION_EQUALITY;
  candidate.selected = 0u;
  candidate.bit_width =
      left_phi->type_kind == QL_IR_TYPE_BOOL ? 1u : left_phi->bit_width;
  candidate.left_loop = left_loop_index;
  candidate.right_loop = right_loop_index;
  candidate.left_phi = phi_index;
  candidate.right_phi = phi_index;
  candidate.left_state = left_phi->result;
  candidate.right_state = right_phi->result;
  candidate.multiplier = UINT64_C(1);
  candidate.offset = UINT64_C(0);
  candidate.left_recurrence = left_phi->recurrence;
  candidate.right_recurrence = right_phi->recurrence;
  if (left_phi->type_kind == QL_IR_TYPE_BOOL) {
    candidate.selected = left_phi->latch_value_count == 1u &&
                                 right_phi->latch_value_count == 1u &&
                                 entry_leaves_equal(&left_entry, &right_entry)
                             ? 1u
                             : 0u;
    return add_candidate(query, &candidate, error);
  }
  if (left_phi->type_kind != QL_IR_TYPE_BIT_VECTOR ||
      left_phi->bit_width == 0u || left_phi->bit_width > 64u ||
      !recurrence_coefficients(left_phi, &left_a, &left_b) ||
      !recurrence_coefficients(right_phi, &right_a, &right_b)) {
    return add_candidate(query, &candidate, error);
  }
  if (entry_leaves_equal(&left_entry, &right_entry) &&
      affine_relation_is_preserved(left_phi->bit_width, left_a, left_b, right_a,
                                   right_b, UINT64_C(1), UINT64_C(0))) {
    candidate.selected = 1u;
    selected = 1u;
  }
  status = add_candidate(query, &candidate, error);
  if (status != QL_STATUS_OK || left_entry.kind != ENTRY_LEAF_CONSTANT ||
      right_entry.kind != ENTRY_LEAF_CONSTANT) {
    return status;
  }
  {
    const uint64_t offset = modular_value(
        left_entry.constant - right_entry.constant, left_phi->bit_width);
    if (offset != 0u &&
        affine_relation_is_preserved(left_phi->bit_width, left_a, left_b,
                                     right_a, right_b, UINT64_C(1), offset)) {
      candidate.kind = QL_LOOP_RELATION_CONSTANT_OFFSET;
      candidate.selected = selected == 0u ? 1u : 0u;
      candidate.multiplier = UINT64_C(1);
      candidate.offset = offset;
      status = add_candidate(query, &candidate, error);
      if (status != QL_STATUS_OK) {
        return status;
      }
      if (candidate.selected != 0u) {
        selected = 1u;
      }
    }
  }
  {
    uint64_t multipliers[5];
    size_t multiplier_index;
    multipliers[0] = UINT64_C(0);
    multipliers[1] = modular_value(left_a, left_phi->bit_width);
    multipliers[2] = modular_value(right_a, left_phi->bit_width);
    multipliers[3] = modular_value(left_a * right_a, left_phi->bit_width);
    multipliers[4] =
        (right_b & UINT64_C(1)) != 0u
            ? modular_value(
                  left_b * odd_modular_inverse(right_b, left_phi->bit_width),
                  left_phi->bit_width)
            : UINT64_C(1);
    for (multiplier_index = 0u; multiplier_index < 5u; ++multiplier_index) {
      const uint64_t multiplier = multipliers[multiplier_index];
      const uint64_t offset =
          modular_value(left_entry.constant - multiplier * right_entry.constant,
                        left_phi->bit_width);
      size_t earlier;
      int duplicate = multiplier == UINT64_C(1);
      for (earlier = 0u; earlier < multiplier_index; ++earlier) {
        if (multipliers[earlier] == multiplier) {
          duplicate = 1;
        }
      }
      if (duplicate ||
          !affine_relation_is_preserved(left_phi->bit_width, left_a, left_b,
                                        right_a, right_b, multiplier, offset)) {
        continue;
      }
      candidate.kind = QL_LOOP_RELATION_AFFINE;
      candidate.selected = selected == 0u ? 1u : 0u;
      candidate.multiplier = multiplier;
      candidate.offset = offset;
      status = add_candidate(query, &candidate, error);
      if (status != QL_STATUS_OK) {
        return status;
      }
      if (candidate.selected != 0u) {
        selected = 1u;
      }
    }
  }
  return QL_STATUS_OK;
}

static int summary_step(const ql_loop_phi_view *phi, uint64_t *step) {
  if (phi->type_kind == QL_IR_TYPE_BOOL) {
    if (phi->recurrence == QL_LOOP_RECURRENCE_IDENTITY) {
      *step = UINT64_C(0);
      return 1;
    }
    return 0;
  }
  if (phi->type_kind != QL_IR_TYPE_BIT_VECTOR || phi->bit_width == 0u ||
      phi->bit_width > 64u) {
    return 0;
  }
  switch (phi->recurrence) {
  case QL_LOOP_RECURRENCE_IDENTITY:
    *step = UINT64_C(0);
    return 1;
  case QL_LOOP_RECURRENCE_ADD_CONSTANT:
    *step = modular_value(phi->step, phi->bit_width);
    return 1;
  case QL_LOOP_RECURRENCE_SUB_CONSTANT:
    *step = modular_value(UINT64_C(0) - phi->step, phi->bit_width);
    return 1;
  default:
    return 0;
  }
}

static uint32_t affine_summary_available(const ql_loop_analysis *left_analysis,
                                         const ql_loop_analysis *right_analysis,
                                         const loop_pair *pairs,
                                         size_t pair_count) {
  size_t pair_index;
  size_t state_count = 0u;

  for (pair_index = 0u; pair_index < pair_count; ++pair_index) {
    const ql_loop_view *left =
        ql_loop_analysis_loop_at(left_analysis, pairs[pair_index].left);
    const ql_loop_view *right =
        ql_loop_analysis_loop_at(right_analysis, pairs[pair_index].right);
    size_t phi_index;
    if (left == NULL || right == NULL || left->phi_count != right->phi_count ||
        left->guard.position == QL_LOOP_GUARD_AMBIGUOUS ||
        right->guard.position == QL_LOOP_GUARD_AMBIGUOUS) {
      return 0;
    }
    for (phi_index = 0u; phi_index < left->phi_count; ++phi_index) {
      uint64_t left_step;
      uint64_t right_step;
      if (!summary_step(&left->phis[phi_index], &left_step) ||
          !summary_step(&right->phis[phi_index], &right_step) ||
          left->phis[phi_index].type_kind != right->phis[phi_index].type_kind ||
          left->phis[phi_index].bit_width != right->phis[phi_index].bit_width ||
          left_step != right_step) {
        return 0;
      }
      ++state_count;
    }
  }
  return state_count != 0u;
}

static ql_status
build_canonical_digest(const ql_ir *left_ir, const ql_ir *right_ir,
                       const ql_loop_analysis *left_analysis,
                       const ql_loop_analysis *right_analysis,
                       const loop_pair *pairs, size_t pair_count,
                       uint32_t ir_structural_match,
                       uint32_t actual_scalar_path, loop_buffer *canonical,
                       ql_digest *digest, ql_error *error) {
  ql_ir_view_v1 left_ir_view;
  ql_ir_view_v1 right_ir_view;
  size_t pair_index;
  ql_status status;

  status = get_ir_view(left_ir, &left_ir_view, error);
  if (status == QL_STATUS_OK) {
    status = get_ir_view(right_ir, &right_ir_view, error);
  }
  if (status == QL_STATUS_OK) {
    status = buffer_addf(
        canonical, error, "loop-proof-canonical-v1\nstructural=%u\npath=%s\n",
        (unsigned)ir_structural_match,
        actual_scalar_path != 0u ? "actual-scalar" : "shared-exact");
  }
  if (status == QL_STATUS_OK) {
    char left_hex[QL_DIGEST_HEX_SIZE];
    char right_hex[QL_DIGEST_HEX_SIZE];
    ql_digest_hex(&left_ir_view.artifact_digest, left_hex);
    ql_digest_hex(&right_ir_view.artifact_digest, right_hex);
    status = buffer_addf(canonical, error, "left=%s\nright=%s\npairs=%zu\n",
                         left_hex, right_hex, pair_count);
  }
  for (pair_index = 0u; pair_index < pair_count && status == QL_STATUS_OK;
       ++pair_index) {
    const ql_loop_view *left =
        ql_loop_analysis_loop_at(left_analysis, pairs[pair_index].left);
    const ql_loop_view *right =
        ql_loop_analysis_loop_at(right_analysis, pairs[pair_index].right);
    size_t phi_index;
    if (left == NULL || right == NULL) {
      ql_error_set(error, QL_STATUS_INTERNAL_ERROR,
                   "paired loop disappeared during canonicalization");
      return QL_STATUS_INTERNAL_ERROR;
    }
    status = buffer_addf(
        canonical, error,
        "pair=%zu,l=%zu,r=%zu,blocks=%zu,latches=%zu,exits=%zu,phis=%zu,"
        "guard=%u,polarity=%u\n",
        pair_index, pairs[pair_index].left, pairs[pair_index].right,
        left->block_count, left->latch_count, left->exit_count, left->phi_count,
        (unsigned)left->guard.position, (unsigned)left->guard.continue_on_true);
    for (phi_index = 0u; phi_index < left->phi_count && status == QL_STATUS_OK;
         ++phi_index) {
      const ql_loop_phi_view *left_phi = &left->phis[phi_index];
      const ql_loop_phi_view *right_phi = &right->phis[phi_index];
      status = buffer_addf(
          canonical, error,
          "phi=%zu,kind=%u,width=%u,lrec=%u,%llu,%llu,%llu,"
          "rrec=%u,%llu,%llu,%llu\n",
          phi_index, (unsigned)left_phi->type_kind,
          (unsigned)left_phi->bit_width, (unsigned)left_phi->recurrence,
          (unsigned long long)left_phi->step,
          (unsigned long long)left_phi->multiplier,
          (unsigned long long)left_phi->offset, (unsigned)right_phi->recurrence,
          (unsigned long long)right_phi->step,
          (unsigned long long)right_phi->multiplier,
          (unsigned long long)right_phi->offset);
    }
  }
  if (status == QL_STATUS_OK) {
    digest_buffer(canonical, digest);
  }
  return status;
}

static ql_status add_phi_sort(loop_buffer *buffer, const ql_loop_phi_view *phi,
                              ql_error *error) {
  switch (phi->type_kind) {
  case QL_IR_TYPE_BOOL:
    return buffer_add(buffer, "Bool", error);
  case QL_IR_TYPE_BIT_VECTOR:
  case QL_IR_TYPE_POINTER:
    return buffer_addf(buffer, error, "(_ BitVec %u)",
                       (unsigned)phi->bit_width);
  case QL_IR_TYPE_MEMORY:
    return buffer_addf(buffer, error, "(Array (_ BitVec %u) (_ BitVec %u))",
                       (unsigned)QL_LOOP_MEMORY_INDEX_WIDTH,
                       (unsigned)QL_LOOP_MEMORY_ELEMENT_WIDTH);
  case QL_IR_TYPE_EVENT_TRACE:
    return buffer_addf(buffer, error, "(_ BitVec %u)",
                       (unsigned)QL_LOOP_EVENT_TRACE_WIDTH);
  default:
    ql_error_set(error, QL_STATUS_TYPE_MISMATCH,
                 "loop state has unsupported type kind %u",
                 (unsigned)phi->type_kind);
    return QL_STATUS_TYPE_MISMATCH;
  }
}

static ql_status add_type_sort(loop_buffer *buffer, const ql_ir *ir,
                               ql_ir_type_id type, ql_error *error) {
  ql_ir_type_view_v1 view;
  ql_status status = get_type_view(ir, type, &view, error);

  if (status != QL_STATUS_OK) {
    return status;
  }
  switch (view.kind) {
  case QL_IR_TYPE_BOOL:
    return buffer_add(buffer, "Bool", error);
  case QL_IR_TYPE_BIT_VECTOR:
    if (view.bit_width == 0u || view.bit_width > 64u) {
      ql_error_set(error, QL_STATUS_TYPE_MISMATCH,
                   "loop expression bit-vector width is outside 1..64");
      return QL_STATUS_TYPE_MISMATCH;
    }
    return buffer_addf(buffer, error, "(_ BitVec %u)",
                       (unsigned)view.bit_width);
  default:
    ql_error_set(error, QL_STATUS_TYPE_MISMATCH,
                 "unsupported loop expression type kind %u",
                 (unsigned)view.kind);
    return QL_STATUS_TYPE_MISMATCH;
  }
}

static int loop_contains_block(const ql_loop_view *loop, ql_ir_block_id block) {
  size_t index;
  for (index = 0u; index < loop->block_count; ++index) {
    if (loop->blocks[index] == block) {
      return 1;
    }
  }
  return 0;
}

typedef struct expression_context {
  const ql_ir *ir;
  const ql_loop_view *loop;
  size_t pair_index;
  char side;
  ql_ir_block_id allowed_exit_block;
  uint32_t entry_leaf_only;
  size_t depth_limit;
  uint32_t *maximum_width;
} expression_context;

static ql_status add_value_expression(loop_buffer *buffer,
                                      const expression_context *context,
                                      ql_ir_value_id value, size_t depth,
                                      ql_error *error);

static ql_status add_constant_expression(loop_buffer *buffer, const ql_ir *ir,
                                         const ql_ir_value_view_v1 *value,
                                         ql_error *error) {
  ql_ir_type_view_v1 type;
  const uint8_t *bytes;
  uint64_t bits = UINT64_C(0);
  size_t index;
  ql_status status = get_type_view(ir, value->type, &type, error);

  if (status != QL_STATUS_OK) {
    return status;
  }
  if (value->constant_data == NULL) {
    ql_error_set(error, QL_STATUS_TYPE_MISMATCH,
                 "loop expression constant has no bytes");
    return QL_STATUS_TYPE_MISMATCH;
  }
  if (type.kind == QL_IR_TYPE_BOOL) {
    if (value->constant_size != 1u) {
      ql_error_set(error, QL_STATUS_TYPE_MISMATCH,
                   "Boolean loop constant is not one byte");
      return QL_STATUS_TYPE_MISMATCH;
    }
    return buffer_add(
        buffer,
        ((const uint8_t *)value->constant_data)[0] != 0u ? "true" : "false",
        error);
  }
  if (type.kind != QL_IR_TYPE_BIT_VECTOR || type.bit_width == 0u ||
      type.bit_width > 64u || value->constant_size > sizeof(bits)) {
    ql_error_set(error, QL_STATUS_TYPE_MISMATCH,
                 "loop expression constant is outside the <=64-bit scalar "
                 "fragment");
    return QL_STATUS_TYPE_MISMATCH;
  }
  bytes = (const uint8_t *)value->constant_data;
  for (index = 0u; index < value->constant_size; ++index) {
    bits |= (uint64_t)bytes[index] << (index * 8u);
  }
  bits = modular_value(bits, type.bit_width);
  return buffer_addf(buffer, error, "(_ bv%llu %u)", (unsigned long long)bits,
                     (unsigned)type.bit_width);
}

static ql_status add_operand_expression(loop_buffer *buffer,
                                        const expression_context *context,
                                        const ql_ir_instruction_view_v1 *view,
                                        size_t operand, size_t depth,
                                        ql_error *error) {
  if (operand >= view->operand_count) {
    ql_error_set(error, QL_STATUS_INTERNAL_ERROR,
                 "loop expression operand is missing");
    return QL_STATUS_INTERNAL_ERROR;
  }
  return add_value_expression(buffer, context, view->operands[operand], depth,
                              error);
}

static ql_status add_unary_expression(loop_buffer *buffer,
                                      const expression_context *context,
                                      const ql_ir_instruction_view_v1 *view,
                                      const char *operator_name, size_t depth,
                                      ql_error *error) {
  ql_status status = buffer_addf(buffer, error, "(%s ", operator_name);
  if (status == QL_STATUS_OK) {
    status = add_operand_expression(buffer, context, view, 0u, depth, error);
  }
  if (status == QL_STATUS_OK) {
    status = buffer_add(buffer, ")", error);
  }
  return status;
}

static ql_status add_binary_expression(loop_buffer *buffer,
                                       const expression_context *context,
                                       const ql_ir_instruction_view_v1 *view,
                                       const char *operator_name, size_t depth,
                                       ql_error *error) {
  ql_status status = buffer_addf(buffer, error, "(%s ", operator_name);
  if (status == QL_STATUS_OK) {
    status = add_operand_expression(buffer, context, view, 0u, depth, error);
  }
  if (status == QL_STATUS_OK) {
    status = buffer_add(buffer, " ", error);
  }
  if (status == QL_STATUS_OK) {
    status = add_operand_expression(buffer, context, view, 1u, depth, error);
  }
  if (status == QL_STATUS_OK) {
    status = buffer_add(buffer, ")", error);
  }
  return status;
}

static ql_status add_value_expression(loop_buffer *buffer,
                                      const expression_context *context,
                                      ql_ir_value_id value, size_t depth,
                                      ql_error *error) {
  ql_ir_value_view_v1 value_view;
  ql_ir_instruction_view_v1 instruction;
  size_t phi_index;
  ql_status status;

  if (depth > context->depth_limit) {
    ql_error_set(error, QL_STATUS_TYPE_MISMATCH,
                 "loop scalar expression exceeds the SSA depth limit");
    return QL_STATUS_TYPE_MISMATCH;
  }
  for (phi_index = 0u; phi_index < context->loop->phi_count; ++phi_index) {
    if (context->loop->phis[phi_index].result == value) {
      if (context->entry_leaf_only != 0u) {
        ql_error_set(error, QL_STATUS_TYPE_MISMATCH,
                     "loop entry depends on loop-carried state");
        return QL_STATUS_TYPE_MISMATCH;
      }
      return buffer_addf(buffer, error, "ql_lp_%c_%zu_%zu", context->side,
                         context->pair_index, phi_index);
    }
  }
  status = get_value_view(context->ir, value, &value_view, error);
  if (status != QL_STATUS_OK) {
    return status;
  }
  {
    ql_ir_type_view_v1 value_type;
    status = get_type_view(context->ir, value_view.type, &value_type, error);
    if (status != QL_STATUS_OK) {
      return status;
    }
    if (value_type.kind != QL_IR_TYPE_BOOL &&
        (value_type.kind != QL_IR_TYPE_BIT_VECTOR ||
         value_type.bit_width == 0u || value_type.bit_width > 64u)) {
      ql_error_set(error, QL_STATUS_TYPE_MISMATCH,
                   "loop expression value is outside the <=64-bit scalar "
                   "fragment");
      return QL_STATUS_TYPE_MISMATCH;
    }
    if (context->maximum_width != NULL &&
        value_type.kind == QL_IR_TYPE_BIT_VECTOR &&
        value_type.bit_width > *context->maximum_width) {
      *context->maximum_width = value_type.bit_width;
    }
  }
  if (value_view.definition_kind == QL_IR_VALUE_PARAMETER) {
    size_t ordinal;
    uint32_t found;
    ql_ir_type_view_v1 parameter_type;
    status =
        get_type_view(context->ir, value_view.type, &parameter_type, error);
    if (status != QL_STATUS_OK) {
      return status;
    }
    if (parameter_type.kind != QL_IR_TYPE_BOOL &&
        parameter_type.kind != QL_IR_TYPE_BIT_VECTOR) {
      ql_error_set(error, QL_STATUS_TYPE_MISMATCH,
                   "loop scalar expression references a non-scalar "
                   "positional parameter");
      return QL_STATUS_TYPE_MISMATCH;
    }
    status = parameter_ordinal(context->ir, value, &ordinal, &found, error);
    if (status != QL_STATUS_OK) {
      return status;
    }
    if (found == 0u) {
      ql_error_set(error, QL_STATUS_INTERNAL_ERROR,
                   "IR parameter has no positional ordinal");
      return QL_STATUS_INTERNAL_ERROR;
    }
    return buffer_addf(buffer, error, "ql_lp_param_%zu", ordinal);
  }
  if (value_view.definition_kind == QL_IR_VALUE_CONSTANT) {
    return add_constant_expression(buffer, context->ir, &value_view, error);
  }
  if (context->entry_leaf_only != 0u ||
      value_view.definition_kind != QL_IR_VALUE_INSTRUCTION_RESULT) {
    ql_error_set(error, QL_STATUS_TYPE_MISMATCH,
                 "loop entry is not a constant or positional parameter");
    return QL_STATUS_TYPE_MISMATCH;
  }
  status = get_instruction_view(context->ir, value_view.instruction,
                                &instruction, error);
  if (status != QL_STATUS_OK) {
    return status;
  }
  if (instruction.effects != QL_IR_EFFECT_NONE ||
      (!loop_contains_block(context->loop, instruction.block) &&
       instruction.block != context->allowed_exit_block) ||
      instruction.block_operand_count != 0u) {
    ql_error_set(error, QL_STATUS_TYPE_MISMATCH,
                 "loop expression depends on an effect, control PHI, or "
                 "mutable value outside its cutpoint");
    return QL_STATUS_TYPE_MISMATCH;
  }
  ++depth;
  switch (instruction.opcode) {
  case QL_IR_OPCODE_IDENTITY:
    return add_operand_expression(buffer, context, &instruction, 0u, depth,
                                  error);
  case QL_IR_OPCODE_BOOL_NOT:
    return add_unary_expression(buffer, context, &instruction, "not", depth,
                                error);
  case QL_IR_OPCODE_BV_NOT:
    return add_unary_expression(buffer, context, &instruction, "bvnot", depth,
                                error);
  case QL_IR_OPCODE_BV_NEG:
    return add_unary_expression(buffer, context, &instruction, "bvneg", depth,
                                error);
  case QL_IR_OPCODE_ADD:
    return add_binary_expression(buffer, context, &instruction, "bvadd", depth,
                                 error);
  case QL_IR_OPCODE_SUB:
    return add_binary_expression(buffer, context, &instruction, "bvsub", depth,
                                 error);
  case QL_IR_OPCODE_MUL:
    return add_binary_expression(buffer, context, &instruction, "bvmul", depth,
                                 error);
  case QL_IR_OPCODE_UDIV:
    return add_binary_expression(buffer, context, &instruction, "bvudiv", depth,
                                 error);
  case QL_IR_OPCODE_SDIV:
    return add_binary_expression(buffer, context, &instruction, "bvsdiv", depth,
                                 error);
  case QL_IR_OPCODE_UREM:
    return add_binary_expression(buffer, context, &instruction, "bvurem", depth,
                                 error);
  case QL_IR_OPCODE_SREM:
    return add_binary_expression(buffer, context, &instruction, "bvsrem", depth,
                                 error);
  case QL_IR_OPCODE_SHL:
    return add_binary_expression(buffer, context, &instruction, "bvshl", depth,
                                 error);
  case QL_IR_OPCODE_LSHR:
    return add_binary_expression(buffer, context, &instruction, "bvlshr", depth,
                                 error);
  case QL_IR_OPCODE_ASHR:
    return add_binary_expression(buffer, context, &instruction, "bvashr", depth,
                                 error);
  case QL_IR_OPCODE_BV_AND:
    return add_binary_expression(buffer, context, &instruction, "bvand", depth,
                                 error);
  case QL_IR_OPCODE_BV_OR:
    return add_binary_expression(buffer, context, &instruction, "bvor", depth,
                                 error);
  case QL_IR_OPCODE_BV_XOR:
    return add_binary_expression(buffer, context, &instruction, "bvxor", depth,
                                 error);
  case QL_IR_OPCODE_EQ:
    return add_binary_expression(buffer, context, &instruction, "=", depth,
                                 error);
  case QL_IR_OPCODE_NE:
    status = buffer_add(buffer, "(not ", error);
    if (status == QL_STATUS_OK) {
      status = add_binary_expression(buffer, context, &instruction, "=", depth,
                                     error);
    }
    if (status == QL_STATUS_OK) {
      status = buffer_add(buffer, ")", error);
    }
    return status;
  case QL_IR_OPCODE_ULT:
    return add_binary_expression(buffer, context, &instruction, "bvult", depth,
                                 error);
  case QL_IR_OPCODE_ULE:
    return add_binary_expression(buffer, context, &instruction, "bvule", depth,
                                 error);
  case QL_IR_OPCODE_SLT:
    return add_binary_expression(buffer, context, &instruction, "bvslt", depth,
                                 error);
  case QL_IR_OPCODE_SLE:
    return add_binary_expression(buffer, context, &instruction, "bvsle", depth,
                                 error);
  case QL_IR_OPCODE_SELECT:
    status = buffer_add(buffer, "(ite ", error);
    if (status == QL_STATUS_OK) {
      status = add_operand_expression(buffer, context, &instruction, 0u, depth,
                                      error);
    }
    if (status == QL_STATUS_OK) {
      status = buffer_add(buffer, " ", error);
    }
    if (status == QL_STATUS_OK) {
      status = add_operand_expression(buffer, context, &instruction, 1u, depth,
                                      error);
    }
    if (status == QL_STATUS_OK) {
      status = buffer_add(buffer, " ", error);
    }
    if (status == QL_STATUS_OK) {
      status = add_operand_expression(buffer, context, &instruction, 2u, depth,
                                      error);
    }
    if (status == QL_STATUS_OK) {
      status = buffer_add(buffer, ")", error);
    }
    return status;
  case QL_IR_OPCODE_ZEXT:
  case QL_IR_OPCODE_SEXT: {
    ql_ir_value_view_v1 operand;
    ql_ir_type_view_v1 operand_type;
    ql_ir_type_view_v1 result_type;
    if (instruction.operand_count < 1u) {
      ql_error_set(error, QL_STATUS_TYPE_MISMATCH,
                   "extension expression has no operand");
      return QL_STATUS_TYPE_MISMATCH;
    }
    status =
        get_value_view(context->ir, instruction.operands[0], &operand, error);
    if (status == QL_STATUS_OK) {
      status = get_type_view(context->ir, operand.type, &operand_type, error);
    }
    if (status == QL_STATUS_OK) {
      status = get_type_view(context->ir, value_view.type, &result_type, error);
    }
    if (status != QL_STATUS_OK ||
        result_type.bit_width < operand_type.bit_width) {
      return status == QL_STATUS_OK ? QL_STATUS_TYPE_MISMATCH : status;
    }
    status = buffer_addf(
        buffer, error, "((_ %s %u) ",
        instruction.opcode == QL_IR_OPCODE_ZEXT ? "zero_extend" : "sign_extend",
        (unsigned)(result_type.bit_width - operand_type.bit_width));
    if (status == QL_STATUS_OK) {
      status = add_operand_expression(buffer, context, &instruction, 0u, depth,
                                      error);
    }
    if (status == QL_STATUS_OK) {
      status = buffer_add(buffer, ")", error);
    }
    return status;
  }
  case QL_IR_OPCODE_TRUNC: {
    ql_ir_type_view_v1 result_type;
    status = get_type_view(context->ir, value_view.type, &result_type, error);
    if (status != QL_STATUS_OK || result_type.bit_width == 0u) {
      return status == QL_STATUS_OK ? QL_STATUS_TYPE_MISMATCH : status;
    }
    status = buffer_addf(buffer, error, "((_ extract %u 0) ",
                         (unsigned)(result_type.bit_width - 1u));
    if (status == QL_STATUS_OK) {
      status = add_operand_expression(buffer, context, &instruction, 0u, depth,
                                      error);
    }
    if (status == QL_STATUS_OK) {
      status = buffer_add(buffer, ")", error);
    }
    return status;
  }
  default:
    ql_error_set(error, QL_STATUS_TYPE_MISMATCH,
                 "opcode %u is outside the pure scalar loop expression DAG",
                 (unsigned)instruction.opcode);
    return QL_STATUS_TYPE_MISMATCH;
  }
}

static ql_status nth_parameter(const ql_ir *ir, size_t wanted,
                               ql_ir_value_view_v1 *output, uint32_t *found,
                               ql_error *error) {
  ql_ir_view_v1 ir_view;
  size_t index;
  size_t ordinal = 0u;
  ql_status status = get_ir_view(ir, &ir_view, error);

  *found = 0u;
  for (index = 0u; status == QL_STATUS_OK && index < ir_view.value_count;
       ++index) {
    ql_ir_value_view_v1 value;
    status = get_value_view(ir, index, &value, error);
    if (status != QL_STATUS_OK) {
      break;
    }
    if (value.definition_kind != QL_IR_VALUE_PARAMETER) {
      continue;
    }
    if (ordinal == wanted) {
      *output = value;
      *found = 1u;
      break;
    }
    ++ordinal;
  }
  return status;
}

static int scalar_types_compatible(const ql_ir_type_view_v1 *left,
                                   const ql_ir_type_view_v1 *right) {
  return left->kind == right->kind && left->bit_width == right->bit_width &&
         (left->kind == QL_IR_TYPE_BOOL ||
          (left->kind == QL_IR_TYPE_BIT_VECTOR && left->bit_width != 0u &&
           left->bit_width <= 64u));
}

static ql_status declare_shared_scalar_parameters(loop_buffer *prefix,
                                                  const ql_ir *left_ir,
                                                  const ql_ir *right_ir,
                                                  uint32_t *maximum_width,
                                                  ql_error *error) {
  ql_ir_view_v1 left_view;
  size_t left_index;
  size_t ordinal = 0u;
  ql_status status = get_ir_view(left_ir, &left_view, error);

  for (left_index = 0u;
       status == QL_STATUS_OK && left_index < left_view.value_count;
       ++left_index) {
    ql_ir_value_view_v1 left_value;
    ql_ir_value_view_v1 right_value;
    ql_ir_type_view_v1 left_type;
    ql_ir_type_view_v1 right_type;
    uint32_t found;

    status = get_value_view(left_ir, left_index, &left_value, error);
    if (status != QL_STATUS_OK) {
      break;
    }
    if (left_value.definition_kind != QL_IR_VALUE_PARAMETER) {
      continue;
    }
    status = nth_parameter(right_ir, ordinal, &right_value, &found, error);
    if (status != QL_STATUS_OK) {
      break;
    }
    if (found == 0u) {
      ql_error_set(error, QL_STATUS_TYPE_MISMATCH,
                   "right IR has fewer positional parameters");
      return QL_STATUS_TYPE_MISMATCH;
    }
    status = get_type_view(left_ir, left_value.type, &left_type, error);
    if (status == QL_STATUS_OK) {
      status = get_type_view(right_ir, right_value.type, &right_type, error);
    }
    if (status != QL_STATUS_OK) {
      break;
    }
    if (!scalar_types_compatible(&left_type, &right_type)) {
      if (left_type.kind == right_type.kind &&
          (left_type.kind == QL_IR_TYPE_MEMORY ||
           left_type.kind == QL_IR_TYPE_EVENT_TRACE ||
           left_type.kind == QL_IR_TYPE_POINTER)) {
        ++ordinal;
        continue;
      }
      ql_error_set(error, QL_STATUS_TYPE_MISMATCH,
                   "positional loop parameters do not share a <=64-bit "
                   "scalar sort");
      return QL_STATUS_TYPE_MISMATCH;
    }
    status =
        buffer_addf(prefix, error, "(declare-fun ql_lp_param_%zu () ", ordinal);
    if (status == QL_STATUS_OK) {
      status = add_type_sort(prefix, left_ir, left_value.type, error);
    }
    if (status == QL_STATUS_OK) {
      status = buffer_add(prefix, ")\n", error);
    }
    if (left_type.kind == QL_IR_TYPE_BIT_VECTOR &&
        left_type.bit_width > *maximum_width) {
      *maximum_width = left_type.bit_width;
    }
    ++ordinal;
  }
  if (status == QL_STATUS_OK) {
    ql_ir_value_view_v1 extra;
    uint32_t found;
    status = nth_parameter(right_ir, ordinal, &extra, &found, error);
    if (status == QL_STATUS_OK && found != 0u) {
      ql_error_set(error, QL_STATUS_TYPE_MISMATCH,
                   "right IR has more positional parameters");
      return QL_STATUS_TYPE_MISMATCH;
    }
  }
  return status;
}

static const ql_loop_relation_candidate_v1 *
selected_candidate(const ql_loop_proof_query *query, size_t left_loop,
                   size_t phi_index) {
  size_t index;
  for (index = 0u; index < query->view.candidate_count; ++index) {
    const ql_loop_relation_candidate_v1 *candidate = &query->candidates[index];
    if (candidate->selected != 0u && candidate->left_loop == left_loop &&
        candidate->left_phi == phi_index) {
      return candidate;
    }
  }
  return NULL;
}

static ql_status
add_candidate_relation(loop_buffer *buffer,
                       const ql_loop_relation_candidate_v1 *candidate,
                       const char *left_kind, const char *right_kind,
                       size_t pair_index, size_t phi_index, ql_error *error) {
  ql_status status;

  if (candidate->kind == QL_LOOP_RELATION_EQUALITY) {
    return buffer_addf(buffer, error, "(= ql_lp_%s_%zu_%zu ql_lp_%s_%zu_%zu)",
                       left_kind, pair_index, phi_index, right_kind, pair_index,
                       phi_index);
  }
  status = buffer_addf(buffer, error,
                       "(= ql_lp_%s_%zu_%zu (bvadd (bvmul (_ bv%llu %u) "
                       "ql_lp_%s_%zu_%zu) (_ bv%llu %u)))",
                       left_kind, pair_index, phi_index,
                       (unsigned long long)candidate->multiplier,
                       (unsigned)candidate->bit_width, right_kind, pair_index,
                       phi_index, (unsigned long long)candidate->offset,
                       (unsigned)candidate->bit_width);
  return status;
}

static ql_status add_state_candidate_relation(
    loop_buffer *buffer, const ql_loop_proof_query *query, size_t left_loop,
    const char *left_kind, const char *right_kind, size_t pair_index,
    const ql_loop_view *loop, uint32_t missing_ok, ql_error *error) {
  size_t phi_index;
  ql_status status = buffer_add(buffer, "(and true", error);

  for (phi_index = 0u; phi_index < loop->phi_count && status == QL_STATUS_OK;
       ++phi_index) {
    const ql_loop_relation_candidate_v1 *candidate =
        selected_candidate(query, left_loop, phi_index);
    if (candidate == NULL) {
      /* With missing_ok the caller keeps the prefix for an external CHC/PDR
         backend, which synthesizes its own invariant; the fixed-vocabulary
         terminals stay meaningless and the disposition says so. */
      if (missing_ok != 0u) {
        continue;
      }
      ql_error_set(error, QL_STATUS_TYPE_MISMATCH,
                   "loop PHI has no Base-and-recurrence-justified relation");
      return QL_STATUS_TYPE_MISMATCH;
    }
    status = buffer_add(buffer, " ", error);
    if (status == QL_STATUS_OK) {
      status = add_candidate_relation(buffer, candidate, left_kind, right_kind,
                                      pair_index, phi_index, error);
    }
  }
  if (status == QL_STATUS_OK) {
    status = buffer_add(buffer, ")", error);
  }
  return status;
}

static void update_maximum_width(const ql_loop_phi_view *phi,
                                 uint32_t *maximum_width) {
  uint32_t width = phi->bit_width;
  if (phi->type_kind == QL_IR_TYPE_BOOL) {
    width = 1u;
  } else if (phi->type_kind == QL_IR_TYPE_MEMORY) {
    width = QL_LOOP_MEMORY_INDEX_WIDTH;
  } else if (phi->type_kind == QL_IR_TYPE_EVENT_TRACE) {
    width = QL_LOOP_EVENT_TRACE_WIDTH;
  }
  if (width > *maximum_width) {
    *maximum_width = width;
  }
}

static ql_status add_state_sorts(loop_buffer *buffer, const ql_loop_view *loop,
                                 ql_error *error) {
  size_t phi_index;
  ql_status status = QL_STATUS_OK;

  for (phi_index = 0u; phi_index < loop->phi_count && status == QL_STATUS_OK;
       ++phi_index) {
    if (phi_index != 0u) {
      status = buffer_add(buffer, " ", error);
    }
    if (status == QL_STATUS_OK) {
      status = add_phi_sort(buffer, &loop->phis[phi_index], error);
    }
  }
  return status;
}

static ql_status add_state_arguments(loop_buffer *buffer, size_t pair_index,
                                     char side, const ql_loop_view *loop,
                                     ql_error *error) {
  size_t phi_index;
  ql_status status = QL_STATUS_OK;

  for (phi_index = 0u; phi_index < loop->phi_count && status == QL_STATUS_OK;
       ++phi_index) {
    status = buffer_addf(buffer, error, " ql_lp_%c_%zu_%zu", side, pair_index,
                         phi_index);
  }
  return status;
}

static ql_status add_application(loop_buffer *buffer, const char *function,
                                 size_t function_index, size_t pair_index,
                                 char side, const ql_loop_view *loop,
                                 ql_error *error) {
  ql_status status;

  if (function_index == QL_LOOP_INVALID_INDEX) {
    status = buffer_addf(buffer, error, "ql_lp_%s_%zu", function, pair_index);
  } else {
    status = buffer_addf(buffer, error, "ql_lp_%s_%zu_%zu", function,
                         pair_index, function_index);
  }
  if (status != QL_STATUS_OK || loop->phi_count == 0u) {
    return status;
  }
  /* Turn the just-written function symbol into an application.  Building the
     application in a scratch buffer would allocate at every use; moving the
     symbol right by one byte is deterministic and bounded by its own text. */
  {
    const char *symbol_start = buffer->data;
    size_t start = buffer->size;
    while (start != 0u && buffer->data[start - 1u] != ' ' &&
           buffer->data[start - 1u] != '(' &&
           buffer->data[start - 1u] != '\n') {
      --start;
    }
    (void)symbol_start;
    status = buffer_reserve(buffer, 1u, error);
    if (status != QL_STATUS_OK) {
      return status;
    }
    memmove(buffer->data + start + 1u, buffer->data + start,
            buffer->size - start + 1u);
    buffer->data[start] = '(';
    ++buffer->size;
  }
  status = add_state_arguments(buffer, pair_index, side, loop, error);
  if (status == QL_STATUS_OK) {
    status = buffer_add(buffer, ")", error);
  }
  return status;
}

static ql_status add_state_equality(loop_buffer *buffer, const char *left_kind,
                                    const char *right_kind, size_t pair_index,
                                    const ql_loop_view *loop, ql_error *error) {
  size_t phi_index;
  ql_status status = buffer_add(buffer, "(and true", error);

  for (phi_index = 0u; phi_index < loop->phi_count && status == QL_STATUS_OK;
       ++phi_index) {
    status = buffer_addf(
        buffer, error, " (= ql_lp_%s_%zu_%zu ql_lp_%s_%zu_%zu)", left_kind,
        pair_index, phi_index, right_kind, pair_index, phi_index);
  }
  if (status == QL_STATUS_OK) {
    status = buffer_add(buffer, ")", error);
  }
  return status;
}

static ql_status declare_state_symbol(loop_buffer *prefix, const char *kind,
                                      size_t pair_index, size_t phi_index,
                                      const ql_loop_phi_view *phi,
                                      ql_error *error) {
  ql_status status =
      buffer_addf(prefix, error, "(declare-fun ql_lp_%s_%zu_%zu () ", kind,
                  pair_index, phi_index);
  if (status == QL_STATUS_OK) {
    status = add_phi_sort(prefix, phi, error);
  }
  if (status == QL_STATUS_OK) {
    status = buffer_add(prefix, ")\n", error);
  }
  return status;
}

static ql_status define_state_alias(loop_buffer *prefix, const char *kind,
                                    size_t pair_index, size_t phi_index,
                                    const ql_loop_phi_view *phi,
                                    const char *body_kind, ql_error *error) {
  ql_status status =
      buffer_addf(prefix, error, "(define-fun ql_lp_%s_%zu_%zu () ", kind,
                  pair_index, phi_index);
  if (status == QL_STATUS_OK) {
    status = add_phi_sort(prefix, phi, error);
  }
  if (status == QL_STATUS_OK) {
    status = buffer_addf(prefix, error, " ql_lp_%s_%zu_%zu)\n", body_kind,
                         pair_index, phi_index);
  }
  return status;
}

static ql_status
declare_transition_function(loop_buffer *prefix, size_t pair_index,
                            size_t phi_index, const ql_loop_view *loop,
                            const ql_loop_phi_view *phi, ql_error *error) {
  ql_status status =
      buffer_addf(prefix, error, "(declare-fun ql_lp_step_%zu_%zu (",
                  pair_index, phi_index);
  if (status == QL_STATUS_OK) {
    status = add_state_sorts(prefix, loop, error);
  }
  if (status == QL_STATUS_OK) {
    status = buffer_add(prefix, ") ", error);
  }
  if (status == QL_STATUS_OK) {
    status = add_phi_sort(prefix, phi, error);
  }
  if (status == QL_STATUS_OK) {
    status = buffer_add(prefix, ")\n", error);
  }
  return status;
}

static ql_status declare_boundary_function(loop_buffer *prefix,
                                           const char *kind, size_t pair_index,
                                           const ql_loop_view *loop,
                                           const char *result_sort,
                                           ql_error *error) {
  ql_status status = buffer_addf(prefix, error, "(declare-fun ql_lp_%s_%zu (",
                                 kind, pair_index);
  if (status == QL_STATUS_OK) {
    status = add_state_sorts(prefix, loop, error);
  }
  if (status == QL_STATUS_OK) {
    status = buffer_addf(prefix, error, ") %s)\n", result_sort);
  }
  return status;
}

static ql_status define_transition_result(loop_buffer *prefix, char side,
                                          size_t pair_index, size_t phi_index,
                                          const ql_loop_view *loop,
                                          const ql_loop_phi_view *phi,
                                          ql_error *error) {
  ql_status status =
      buffer_addf(prefix, error, "(define-fun ql_lp_%c_next_%zu_%zu () ", side,
                  pair_index, phi_index);
  if (status == QL_STATUS_OK) {
    status = add_phi_sort(prefix, phi, error);
  }
  if (status == QL_STATUS_OK) {
    status = buffer_add(prefix, " ", error);
  }
  if (status == QL_STATUS_OK) {
    status = add_application(prefix, "step", phi_index, pair_index, side, loop,
                             error);
  }
  if (status == QL_STATUS_OK) {
    status = buffer_add(prefix, ")\n", error);
  }
  return status;
}

static ql_status define_actual_entry(loop_buffer *prefix, const ql_ir *ir,
                                     const ql_loop_view *loop, char side,
                                     size_t pair_index, size_t phi_index,
                                     uint32_t *maximum_width, ql_error *error) {
  expression_context context;
  ql_ir_view_v1 ir_view;
  const ql_loop_phi_view *phi = &loop->phis[phi_index];
  ql_status status = get_ir_view(ir, &ir_view, error);

  if (status != QL_STATUS_OK) {
    return status;
  }
  memset(&context, 0, sizeof(context));
  context.ir = ir;
  context.loop = loop;
  context.pair_index = pair_index;
  context.side = side;
  context.allowed_exit_block = QL_IR_INVALID_BLOCK_ID;
  context.entry_leaf_only = 1u;
  context.depth_limit = ir_view.value_count;
  context.maximum_width = maximum_width;
  status = buffer_addf(prefix, error, "(define-fun ql_lp_%c_entry_%zu_%zu () ",
                       side, pair_index, phi_index);
  if (status == QL_STATUS_OK) {
    status = add_phi_sort(prefix, phi, error);
  }
  if (status == QL_STATUS_OK) {
    status = buffer_add(prefix, " ", error);
  }
  if (status == QL_STATUS_OK) {
    status =
        add_value_expression(prefix, &context, phi->entry_value, 0u, error);
  }
  if (status == QL_STATUS_OK) {
    status = buffer_add(prefix, ")\n", error);
  }
  return status;
}

static ql_status
define_actual_recurrence(loop_buffer *prefix, const ql_ir *ir,
                         const ql_loop_view *loop, const ql_loop_phi_view *phi,
                         char side, size_t pair_index, size_t phi_index,
                         uint32_t *maximum_width, ql_error *error) {
  expression_context context;
  ql_ir_view_v1 ir_view;
  ql_status status =
      buffer_addf(prefix, error, "(define-fun ql_lp_%c_next_%zu_%zu () ", side,
                  pair_index, phi_index);

  if (status == QL_STATUS_OK) {
    status = add_phi_sort(prefix, phi, error);
  }
  if (status == QL_STATUS_OK) {
    status = buffer_add(prefix, " ", error);
  }
  if (status != QL_STATUS_OK) {
    return status;
  }
  if (phi->latch_value_count != 1u ||
      (phi->type_kind != QL_IR_TYPE_BOOL &&
       phi->type_kind != QL_IR_TYPE_BIT_VECTOR) ||
      phi->bit_width > 64u) {
    ql_error_set(error, QL_STATUS_TYPE_MISMATCH,
                 "loop PHI does not have one exact scalar latch expression");
    return QL_STATUS_TYPE_MISMATCH;
  }
  status = get_ir_view(ir, &ir_view, error);
  if (status != QL_STATUS_OK) {
    return status;
  }
  memset(&context, 0, sizeof(context));
  context.ir = ir;
  context.loop = loop;
  context.pair_index = pair_index;
  context.side = side;
  context.allowed_exit_block = QL_IR_INVALID_BLOCK_ID;
  context.depth_limit = ir_view.value_count;
  context.maximum_width = maximum_width;
  status =
      add_value_expression(prefix, &context, phi->latch_values[0], 0u, error);
  if (status == QL_STATUS_OK) {
    status = buffer_add(prefix, ")\n", error);
  }
  return status;
}

static ql_status define_actual_guard(loop_buffer *prefix, const ql_ir *ir,
                                     const ql_loop_view *loop, char side,
                                     size_t pair_index, uint32_t *maximum_width,
                                     ql_error *error) {
  expression_context context;
  ql_ir_view_v1 ir_view;
  loop_buffer expression;
  ql_status status = get_ir_view(ir, &ir_view, error);

  if (status != QL_STATUS_OK) {
    return status;
  }
  buffer_init(&expression, &prefix->allocator);
  memset(&context, 0, sizeof(context));
  context.ir = ir;
  context.loop = loop;
  context.pair_index = pair_index;
  context.side = side;
  context.allowed_exit_block = QL_IR_INVALID_BLOCK_ID;
  context.depth_limit = ir_view.value_count;
  context.maximum_width = maximum_width;
  status = add_value_expression(&expression, &context, loop->guard.condition,
                                0u, error);
  if (status != QL_STATUS_OK) {
    buffer_dispose(&expression);
    return status;
  }
  status = buffer_addf(prefix, error, "(define-fun ql_lp_%c_guard_%zu () Bool ",
                       side, pair_index);
  if (status == QL_STATUS_OK && loop->guard.continue_on_true == 0u) {
    status = buffer_add(prefix, "(not ", error);
  }
  if (status == QL_STATUS_OK) {
    status = buffer_add_n(prefix, expression.data, expression.size, error);
  }
  if (status == QL_STATUS_OK && loop->guard.continue_on_true == 0u) {
    status = buffer_add(prefix, ")", error);
  }
  if (status == QL_STATUS_OK) {
    status = buffer_add(prefix, ")\n", error);
  }
  buffer_dispose(&expression);
  return status;
}

static ql_status define_actual_exit(loop_buffer *prefix, const ql_ir *ir,
                                    const ql_loop_view *loop, char side,
                                    size_t pair_index, uint32_t *maximum_width,
                                    ql_error *error) {
  expression_context context;
  ql_ir_view_v1 ir_view;
  ql_ir_block_view_v1 exit_block;
  ql_ir_value_view_v1 return_value;
  ql_status status;

  if (loop->exit_count != 1u) {
    ql_error_set(error, QL_STATUS_TYPE_MISMATCH,
                 "actual scalar loop path requires one direct exit");
    return QL_STATUS_TYPE_MISMATCH;
  }
  status = get_ir_view(ir, &ir_view, error);
  if (status == QL_STATUS_OK) {
    status = get_block_view(ir, loop->exits[0].to, &exit_block, error);
  }
  if (status != QL_STATUS_OK) {
    return status;
  }
  if (exit_block.terminator.kind != QL_IR_TERMINATOR_RETURN) {
    ql_error_set(error, QL_STATUS_TYPE_MISMATCH,
                 "loop exit is not a direct return block");
    return QL_STATUS_TYPE_MISMATCH;
  }
  if (exit_block.terminator.return_value == QL_IR_INVALID_VALUE_ID) {
    return buffer_addf(prefix, error,
                       "(define-fun ql_lp_%c_exit_%zu () Bool true)\n", side,
                       pair_index);
  }
  status = get_value_view(ir, exit_block.terminator.return_value, &return_value,
                          error);
  if (status != QL_STATUS_OK) {
    return status;
  }
  memset(&context, 0, sizeof(context));
  context.ir = ir;
  context.loop = loop;
  context.pair_index = pair_index;
  context.side = side;
  context.allowed_exit_block = exit_block.id;
  context.depth_limit = ir_view.value_count;
  context.maximum_width = maximum_width;
  status = buffer_addf(prefix, error, "(define-fun ql_lp_%c_exit_%zu () ", side,
                       pair_index);
  if (status == QL_STATUS_OK) {
    status = add_type_sort(prefix, ir, return_value.type, error);
  }
  if (status == QL_STATUS_OK) {
    status = buffer_add(prefix, " ", error);
  }
  if (status == QL_STATUS_OK) {
    status = add_value_expression(
        prefix, &context, exit_block.terminator.return_value, 0u, error);
  }
  if (status == QL_STATUS_OK) {
    status = buffer_add(prefix, ")\n", error);
  }
  return status;
}

static ql_status define_boundary_result(loop_buffer *prefix, const char *kind,
                                        char side, size_t pair_index,
                                        const ql_loop_view *loop,
                                        const char *sort, ql_error *error) {
  ql_status status =
      buffer_addf(prefix, error, "(define-fun ql_lp_%c_%s_%zu () %s ", side,
                  kind, pair_index, sort);
  if (status == QL_STATUS_OK) {
    status = add_application(prefix, kind, QL_LOOP_INVALID_INDEX, pair_index,
                             side, loop, error);
  }
  if (status == QL_STATUS_OK) {
    status = buffer_add(prefix, ")\n", error);
  }
  return status;
}

static ql_status build_structural_loop_terms(
    loop_buffer *prefix, loop_buffer *base_terms, loop_buffer *guard_terms,
    loop_buffer *step_terms, loop_buffer *exit_terms, size_t pair_index,
    const ql_loop_view *loop, uint32_t *maximum_width, ql_error *error) {
  size_t phi_index;
  ql_status status = QL_STATUS_OK;

  for (phi_index = 0u; phi_index < loop->phi_count && status == QL_STATUS_OK;
       ++phi_index) {
    const ql_loop_phi_view *phi = &loop->phis[phi_index];
    update_maximum_width(phi, maximum_width);
    status = declare_state_symbol(prefix, "entry", pair_index, phi_index, phi,
                                  error);
    if (status == QL_STATUS_OK) {
      status =
          declare_state_symbol(prefix, "l", pair_index, phi_index, phi, error);
    }
    if (status == QL_STATUS_OK) {
      status =
          declare_state_symbol(prefix, "r", pair_index, phi_index, phi, error);
    }
  }
  for (phi_index = 0u; phi_index < loop->phi_count && status == QL_STATUS_OK;
       ++phi_index) {
    status = declare_transition_function(prefix, pair_index, phi_index, loop,
                                         &loop->phis[phi_index], error);
  }
  if (status == QL_STATUS_OK) {
    status = declare_boundary_function(prefix, "guard", pair_index, loop,
                                       "Bool", error);
  }
  if (status == QL_STATUS_OK) {
    status = declare_boundary_function(prefix, "exit", pair_index, loop,
                                       "(_ BitVec 1)", error);
  }
  for (phi_index = 0u; phi_index < loop->phi_count && status == QL_STATUS_OK;
       ++phi_index) {
    const ql_loop_phi_view *phi = &loop->phis[phi_index];
    status = define_state_alias(prefix, "l_entry", pair_index, phi_index, phi,
                                "entry", error);
    if (status == QL_STATUS_OK) {
      status = define_state_alias(prefix, "r_entry", pair_index, phi_index, phi,
                                  "entry", error);
    }
    if (status == QL_STATUS_OK) {
      status = define_transition_result(prefix, 'l', pair_index, phi_index,
                                        loop, phi, error);
    }
    if (status == QL_STATUS_OK) {
      status = define_transition_result(prefix, 'r', pair_index, phi_index,
                                        loop, phi, error);
    }
  }
  if (status == QL_STATUS_OK) {
    status = buffer_addf(
        prefix, error, "(define-fun ql_lp_entry_inv_%zu () Bool ", pair_index);
  }
  if (status == QL_STATUS_OK) {
    status = add_state_equality(prefix, "l_entry", "r_entry", pair_index, loop,
                                error);
  }
  if (status == QL_STATUS_OK) {
    status = buffer_add(prefix, ")\n", error);
  }
  if (status == QL_STATUS_OK) {
    status = buffer_addf(prefix, error, "(define-fun ql_lp_inv_%zu () Bool ",
                         pair_index);
  }
  if (status == QL_STATUS_OK) {
    status = add_state_equality(prefix, "l", "r", pair_index, loop, error);
  }
  if (status == QL_STATUS_OK) {
    status = buffer_add(prefix, ")\n", error);
  }
  if (status == QL_STATUS_OK) {
    status = buffer_addf(prefix, error,
                         "(define-fun ql_lp_next_inv_%zu () Bool ", pair_index);
  }
  if (status == QL_STATUS_OK) {
    status =
        add_state_equality(prefix, "l_next", "r_next", pair_index, loop, error);
  }
  if (status == QL_STATUS_OK) {
    status = buffer_add(prefix, ")\n", error);
  }
  if (status == QL_STATUS_OK) {
    status = define_boundary_result(prefix, "guard", 'l', pair_index, loop,
                                    "Bool", error);
  }
  if (status == QL_STATUS_OK) {
    status = define_boundary_result(prefix, "guard", 'r', pair_index, loop,
                                    "Bool", error);
  }
  if (status == QL_STATUS_OK) {
    status = define_boundary_result(prefix, "exit", 'l', pair_index, loop,
                                    "(_ BitVec 1)", error);
  }
  if (status == QL_STATUS_OK) {
    status = define_boundary_result(prefix, "exit", 'r', pair_index, loop,
                                    "(_ BitVec 1)", error);
  }
  if (status == QL_STATUS_OK) {
    status = buffer_addf(prefix, error,
                         "(define-fun ql_lp_base_bad_%zu () Bool "
                         "(not ql_lp_entry_inv_%zu))\n",
                         pair_index, pair_index);
  }
  if (status == QL_STATUS_OK) {
    status = buffer_addf(prefix, error,
                         "(define-fun ql_lp_guard_bad_%zu () Bool "
                         "(and ql_lp_inv_%zu (not (= ql_lp_l_guard_%zu "
                         "ql_lp_r_guard_%zu))))\n",
                         pair_index, pair_index, pair_index, pair_index);
  }
  if (status == QL_STATUS_OK) {
    status =
        buffer_addf(prefix, error,
                    "(define-fun ql_lp_step_bad_%zu () Bool "
                    "(and ql_lp_inv_%zu ql_lp_l_guard_%zu ql_lp_r_guard_%zu "
                    "(not ql_lp_next_inv_%zu)))\n",
                    pair_index, pair_index, pair_index, pair_index, pair_index);
  }
  if (status == QL_STATUS_OK) {
    status = buffer_addf(prefix, error,
                         "(define-fun ql_lp_exit_bad_%zu () Bool "
                         "(and ql_lp_inv_%zu (not ql_lp_l_guard_%zu) "
                         "(not ql_lp_r_guard_%zu) "
                         "(not (= ql_lp_l_exit_%zu ql_lp_r_exit_%zu))))\n",
                         pair_index, pair_index, pair_index, pair_index,
                         pair_index, pair_index);
  }
  if (status == QL_STATUS_OK) {
    status = buffer_addf(base_terms, error, " ql_lp_base_bad_%zu", pair_index);
  }
  if (status == QL_STATUS_OK) {
    status =
        buffer_addf(guard_terms, error, " ql_lp_guard_bad_%zu", pair_index);
  }
  if (status == QL_STATUS_OK) {
    status = buffer_addf(step_terms, error, " ql_lp_step_bad_%zu", pair_index);
  }
  if (status == QL_STATUS_OK) {
    status = buffer_addf(exit_terms, error, " ql_lp_exit_bad_%zu", pair_index);
  }
  return status;
}

static ql_status build_actual_scalar_loop_terms(
    ql_loop_proof_query *query, loop_buffer *prefix, loop_buffer *base_terms,
    loop_buffer *guard_terms, loop_buffer *step_terms, loop_buffer *exit_terms,
    const ql_ir *left_ir, const ql_ir *right_ir, size_t left_loop_index,
    size_t pair_index, const ql_loop_view *left, const ql_loop_view *right,
    uint32_t *maximum_width, uint32_t missing_candidate_ok, ql_error *error) {
  size_t phi_index;
  ql_status status = QL_STATUS_OK;

  for (phi_index = 0u; phi_index < left->phi_count && status == QL_STATUS_OK;
       ++phi_index) {
    update_maximum_width(&left->phis[phi_index], maximum_width);
    status = declare_state_symbol(prefix, "l", pair_index, phi_index,
                                  &left->phis[phi_index], error);
    if (status == QL_STATUS_OK) {
      status = declare_state_symbol(prefix, "r", pair_index, phi_index,
                                    &right->phis[phi_index], error);
    }
    if (status == QL_STATUS_OK) {
      status = define_actual_entry(prefix, left_ir, left, 'l', pair_index,
                                   phi_index, maximum_width, error);
    }
    if (status == QL_STATUS_OK) {
      status = define_actual_entry(prefix, right_ir, right, 'r', pair_index,
                                   phi_index, maximum_width, error);
    }
    if (status == QL_STATUS_OK) {
      status = define_actual_recurrence(prefix, left_ir, left,
                                        &left->phis[phi_index], 'l', pair_index,
                                        phi_index, maximum_width, error);
    }
    if (status == QL_STATUS_OK) {
      status = define_actual_recurrence(
          prefix, right_ir, right, &right->phis[phi_index], 'r', pair_index,
          phi_index, maximum_width, error);
    }
  }
  if (status == QL_STATUS_OK) {
    status = buffer_addf(
        prefix, error, "(define-fun ql_lp_entry_inv_%zu () Bool ", pair_index);
  }
  if (status == QL_STATUS_OK) {
    status =
        add_state_candidate_relation(prefix, query, left_loop_index, "l_entry",
                                     "r_entry", pair_index, left,
                                     missing_candidate_ok, error);
  }
  if (status == QL_STATUS_OK) {
    status = buffer_add(prefix, ")\n", error);
  }
  if (status == QL_STATUS_OK) {
    status = buffer_addf(prefix, error, "(define-fun ql_lp_inv_%zu () Bool ",
                         pair_index);
  }
  if (status == QL_STATUS_OK) {
    status = add_state_candidate_relation(prefix, query, left_loop_index, "l",
                                          "r", pair_index, left,
                                          missing_candidate_ok, error);
  }
  if (status == QL_STATUS_OK) {
    status = buffer_add(prefix, ")\n", error);
  }
  if (status == QL_STATUS_OK) {
    status = buffer_addf(prefix, error,
                         "(define-fun ql_lp_next_inv_%zu () Bool ", pair_index);
  }
  if (status == QL_STATUS_OK) {
    status =
        add_state_candidate_relation(prefix, query, left_loop_index, "l_next",
                                     "r_next", pair_index, left,
                                     missing_candidate_ok, error);
  }
  if (status == QL_STATUS_OK) {
    status = buffer_add(prefix, ")\n", error);
  }
  if (status == QL_STATUS_OK) {
    status = define_actual_guard(prefix, left_ir, left, 'l', pair_index,
                                 maximum_width, error);
  }
  if (status == QL_STATUS_OK) {
    status = define_actual_guard(prefix, right_ir, right, 'r', pair_index,
                                 maximum_width, error);
  }
  if (status == QL_STATUS_OK) {
    status = define_actual_exit(prefix, left_ir, left, 'l', pair_index,
                                maximum_width, error);
  }
  if (status == QL_STATUS_OK) {
    status = define_actual_exit(prefix, right_ir, right, 'r', pair_index,
                                maximum_width, error);
  }
  if (status == QL_STATUS_OK) {
    status = buffer_addf(prefix, error,
                         "(define-fun ql_lp_base_bad_%zu () Bool "
                         "(not ql_lp_entry_inv_%zu))\n",
                         pair_index, pair_index);
  }
  if (status == QL_STATUS_OK) {
    status = buffer_addf(prefix, error,
                         "(define-fun ql_lp_guard_bad_%zu () Bool "
                         "(and ql_lp_inv_%zu (not (= ql_lp_l_guard_%zu "
                         "ql_lp_r_guard_%zu))))\n",
                         pair_index, pair_index, pair_index, pair_index);
  }
  if (status == QL_STATUS_OK) {
    status =
        buffer_addf(prefix, error,
                    "(define-fun ql_lp_step_bad_%zu () Bool "
                    "(and ql_lp_inv_%zu ql_lp_l_guard_%zu ql_lp_r_guard_%zu "
                    "(not ql_lp_next_inv_%zu)))\n",
                    pair_index, pair_index, pair_index, pair_index, pair_index);
  }
  if (status == QL_STATUS_OK) {
    status = buffer_addf(prefix, error,
                         "(define-fun ql_lp_exit_bad_%zu () Bool "
                         "(and ql_lp_inv_%zu (not ql_lp_l_guard_%zu) "
                         "(not ql_lp_r_guard_%zu) "
                         "(not (= ql_lp_l_exit_%zu ql_lp_r_exit_%zu))))\n",
                         pair_index, pair_index, pair_index, pair_index,
                         pair_index, pair_index);
  }
  if (status == QL_STATUS_OK) {
    status = buffer_addf(base_terms, error, " ql_lp_base_bad_%zu", pair_index);
  }
  if (status == QL_STATUS_OK) {
    status =
        buffer_addf(guard_terms, error, " ql_lp_guard_bad_%zu", pair_index);
  }
  if (status == QL_STATUS_OK) {
    status = buffer_addf(step_terms, error, " ql_lp_step_bad_%zu", pair_index);
  }
  if (status == QL_STATUS_OK) {
    status = buffer_addf(exit_terms, error, " ql_lp_exit_bad_%zu", pair_index);
  }
  return status;
}

static ql_status define_summary_value(loop_buffer *prefix, char side,
                                      size_t pair_index, size_t phi_index,
                                      const ql_loop_phi_view *phi,
                                      uint64_t step, ql_error *error) {
  ql_status status =
      buffer_addf(prefix, error, "(define-fun ql_lp_%c_summary_%zu_%zu () ",
                  side, pair_index, phi_index);
  if (status == QL_STATUS_OK) {
    status = add_phi_sort(prefix, phi, error);
  }
  if (status != QL_STATUS_OK) {
    return status;
  }
  if (phi->type_kind == QL_IR_TYPE_BOOL || step == UINT64_C(0)) {
    return buffer_addf(prefix, error, " ql_lp_entry_%zu_%zu)\n", pair_index,
                       phi_index);
  }
  return buffer_addf(prefix, error,
                     " (bvadd ql_lp_entry_%zu_%zu "
                     "(bvmul ql_lp_n_%zu_%zu (_ bv%llu %u))))\n",
                     pair_index, phi_index, pair_index, phi_index,
                     (unsigned long long)step, (unsigned)phi->bit_width);
}

static ql_status
build_summary_loop_terms(loop_buffer *prefix, loop_buffer *summary_terms,
                         size_t pair_index, const ql_loop_view *left,
                         const ql_loop_view *right, ql_error *error) {
  size_t phi_index;
  ql_status status = QL_STATUS_OK;

  for (phi_index = 0u; phi_index < left->phi_count && status == QL_STATUS_OK;
       ++phi_index) {
    uint64_t left_step;
    uint64_t right_step;
    if (!summary_step(&left->phis[phi_index], &left_step) ||
        !summary_step(&right->phis[phi_index], &right_step) ||
        left_step != right_step) {
      ql_error_set(error, QL_STATUS_INTERNAL_ERROR,
                   "summary eligibility changed while building loop %zu",
                   pair_index);
      return QL_STATUS_INTERNAL_ERROR;
    }
    if (left->phis[phi_index].type_kind == QL_IR_TYPE_BIT_VECTOR) {
      status = buffer_addf(
          prefix, error, "(declare-fun ql_lp_n_%zu_%zu () (_ BitVec %u))\n",
          pair_index, phi_index, (unsigned)left->phis[phi_index].bit_width);
    }
    if (status == QL_STATUS_OK) {
      status = define_summary_value(prefix, 'l', pair_index, phi_index,
                                    &left->phis[phi_index], left_step, error);
    }
    if (status == QL_STATUS_OK) {
      status = define_summary_value(prefix, 'r', pair_index, phi_index,
                                    &right->phis[phi_index], right_step, error);
    }
  }
  if (status == QL_STATUS_OK) {
    status = buffer_addf(prefix, error,
                         "(define-fun ql_lp_summary_bad_%zu () Bool "
                         "(not (and true",
                         pair_index);
  }
  for (phi_index = 0u; phi_index < left->phi_count && status == QL_STATUS_OK;
       ++phi_index) {
    status = buffer_addf(prefix, error,
                         " (= ql_lp_l_summary_%zu_%zu "
                         "ql_lp_r_summary_%zu_%zu)",
                         pair_index, phi_index, pair_index, phi_index);
  }
  if (status == QL_STATUS_OK) {
    status = buffer_add(prefix, ")))\n", error);
  }
  if (status == QL_STATUS_OK) {
    status =
        buffer_addf(summary_terms, error, " ql_lp_summary_bad_%zu", pair_index);
  }
  return status;
}

static ql_status scan_whole_ir(const ql_ir *ir, loop_scan *scan,
                               ql_error *error) {
  ql_ir_view_v1 ir_view;
  size_t block_index;
  size_t value_index;
  ql_status status;

  memset(scan, 0, sizeof(*scan));
  status = get_ir_view(ir, &ir_view, error);
  for (block_index = 0u;
       status == QL_STATUS_OK && block_index < ir_view.block_count;
       ++block_index) {
    ql_ir_block_view_v1 block;
    size_t instruction_index;
    status = get_block_view(ir, block_index, &block, error);
    if (status != QL_STATUS_OK) {
      break;
    }
    if (block.terminator.kind == QL_IR_TERMINATOR_UNDEFINED_BEHAVIOR) {
      scan->has_ub = 1u;
    }
    for (instruction_index = 0u;
         instruction_index < block.instruction_count && status == QL_STATUS_OK;
         ++instruction_index) {
      ql_ir_instruction_view_v1 instruction;
      status = get_instruction_view(ir, block.instructions[instruction_index],
                                    &instruction, error);
      if (status != QL_STATUS_OK) {
        break;
      }
      scan->effects |= instruction.effects;
      switch (instruction.opcode) {
      case QL_IR_OPCODE_ASSUME:
        scan->has_assume = 1u;
        break;
      case QL_IR_OPCODE_UB_GUARD:
        scan->has_ub = 1u;
        break;
      case QL_IR_OPCODE_CALL:
        scan->has_call = 1u;
        scan->has_memory = 1u;
        scan->has_event_trace = 1u;
        break;
      case QL_IR_OPCODE_LOAD:
      case QL_IR_OPCODE_STORE:
      case QL_IR_OPCODE_MEMORY_IMAGE:
        scan->has_memory = 1u;
        break;
      case QL_IR_OPCODE_TRACE_APPEND:
        scan->has_event_trace = 1u;
        break;
      default:
        break;
      }
    }
  }
  for (value_index = 0u;
       status == QL_STATUS_OK && value_index < ir_view.value_count;
       ++value_index) {
    ql_ir_value_view_v1 value;
    ql_ir_type_view_v1 type;
    status = get_value_view(ir, value_index, &value, error);
    if (status == QL_STATUS_OK) {
      status = get_type_view(ir, value.type, &type, error);
    }
    if (status != QL_STATUS_OK) {
      break;
    }
    if (type.kind == QL_IR_TYPE_POINTER) {
      scan->has_pointer = 1u;
    } else if (type.kind == QL_IR_TYPE_MEMORY) {
      scan->has_memory = 1u;
    } else if (type.kind == QL_IR_TYPE_EVENT_TRACE) {
      scan->has_event_trace = 1u;
    } else if (type.kind != QL_IR_TYPE_BOOL &&
               (type.kind != QL_IR_TYPE_BIT_VECTOR || type.bit_width == 0u ||
                type.bit_width > 64u)) {
      scan->has_unsupported_type = 1u;
    }
  }
  return status;
}

static ql_status actual_program_shape_supported(const ql_ir *ir,
                                                const ql_loop_view *loop,
                                                uint32_t *supported,
                                                ql_error *error) {
  ql_ir_view_v1 ir_view;
  ql_ir_block_view_v1 preheader;
  ql_ir_block_id current;
  size_t block_index;
  size_t entry_step;
  size_t return_count = 0u;
  uint32_t entry_reaches_preheader = 0u;
  ql_status status;

  *supported = 0u;
  if (loop->exit_count != 1u || loop->preheader == QL_IR_INVALID_BLOCK_ID) {
    return QL_STATUS_OK;
  }
  status = get_ir_view(ir, &ir_view, error);
  if (status == QL_STATUS_OK) {
    status = get_block_view(ir, loop->preheader, &preheader, error);
  }
  if (status != QL_STATUS_OK) {
    return status;
  }
  if (preheader.terminator.kind != QL_IR_TERMINATOR_BRANCH ||
      preheader.terminator.target != loop->header) {
    return QL_STATUS_OK;
  }
  current = ir_view.entry_block;
  for (entry_step = 0u; entry_step <= ir_view.block_count; ++entry_step) {
    ql_ir_block_view_v1 block;
    if (current == loop->preheader) {
      entry_reaches_preheader = 1u;
      break;
    }
    if (current == loop->exits[0].to || loop_contains_block(loop, current)) {
      return QL_STATUS_OK;
    }
    status = get_block_view(ir, current, &block, error);
    if (status != QL_STATUS_OK) {
      return status;
    }
    if (block.terminator.kind != QL_IR_TERMINATOR_BRANCH) {
      return QL_STATUS_OK;
    }
    current = block.terminator.target;
  }
  if (entry_reaches_preheader == 0u) {
    return QL_STATUS_OK;
  }
  for (block_index = 0u;
       status == QL_STATUS_OK && block_index < ir_view.block_count;
       ++block_index) {
    ql_ir_block_view_v1 block;
    status = get_block_view(ir, block_index, &block, error);
    if (status != QL_STATUS_OK) {
      break;
    }
    if (block.terminator.kind == QL_IR_TERMINATOR_RETURN) {
      ++return_count;
    }
    if (loop_contains_block(loop, block.id)) {
      if (block.terminator.kind != QL_IR_TERMINATOR_BRANCH &&
          block.terminator.kind != QL_IR_TERMINATOR_COND_BRANCH) {
        return QL_STATUS_OK;
      }
      continue;
    }
    if (block.id == loop->exits[0].to) {
      if (block.terminator.kind != QL_IR_TERMINATOR_RETURN) {
        return QL_STATUS_OK;
      }
    } else if (block.terminator.kind != QL_IR_TERMINATOR_BRANCH) {
      /* No conditional bypass, early return, trap, divergence, or secondary
         terminal is left outside the one loop proved below. */
      return QL_STATUS_OK;
    }
  }
  if (status == QL_STATUS_OK && return_count == 1u) {
    *supported = 1u;
  }
  return status;
}

static ql_status
actual_exit_types_compatible(const ql_ir *left_ir, const ql_loop_view *left,
                             const ql_ir *right_ir, const ql_loop_view *right,
                             uint32_t *compatible, ql_error *error) {
  ql_ir_block_view_v1 left_exit;
  ql_ir_block_view_v1 right_exit;
  ql_ir_value_view_v1 left_value;
  ql_ir_value_view_v1 right_value;
  ql_ir_type_view_v1 left_type;
  ql_ir_type_view_v1 right_type;
  ql_status status;

  *compatible = 0u;
  if (left->exit_count != 1u || right->exit_count != 1u) {
    return QL_STATUS_OK;
  }
  status = get_block_view(left_ir, left->exits[0].to, &left_exit, error);
  if (status == QL_STATUS_OK) {
    status = get_block_view(right_ir, right->exits[0].to, &right_exit, error);
  }
  if (status != QL_STATUS_OK) {
    return status;
  }
  if (left_exit.terminator.kind != QL_IR_TERMINATOR_RETURN ||
      right_exit.terminator.kind != QL_IR_TERMINATOR_RETURN) {
    return QL_STATUS_OK;
  }
  if (left_exit.terminator.return_value == QL_IR_INVALID_VALUE_ID ||
      right_exit.terminator.return_value == QL_IR_INVALID_VALUE_ID) {
    *compatible =
        left_exit.terminator.return_value == right_exit.terminator.return_value;
    return QL_STATUS_OK;
  }
  status = get_value_view(left_ir, left_exit.terminator.return_value,
                          &left_value, error);
  if (status == QL_STATUS_OK) {
    status = get_value_view(right_ir, right_exit.terminator.return_value,
                            &right_value, error);
  }
  if (status == QL_STATUS_OK) {
    status = get_type_view(left_ir, left_value.type, &left_type, error);
  }
  if (status == QL_STATUS_OK) {
    status = get_type_view(right_ir, right_value.type, &right_type, error);
  }
  if (status == QL_STATUS_OK) {
    *compatible = scalar_types_compatible(&left_type, &right_type) ? 1u : 0u;
  }
  return status;
}

static void mark_fallback(ql_loop_proof_query *query,
                          ql_loop_proof_unsupported_reason reason,
                          const char *diagnostic) {
  query->view.disposition = QL_LOOP_PROOF_CHC_PDR_UNAVAILABLE;
  query->view.strategy = QL_LOOP_PROOF_STRATEGY_CHC_PDR_UNAVAILABLE;
  query->view.unsupported_reason = reason;
  query->view.chc_pdr_reached = 1u;
  query->view.chc_pdr_available = 0u;
  query->view.promotion_eligible = 0u;
  query->view.metrics.fallback_reached_count = 1u;
  query->view.metrics.stage_reached |= QL_LOOP_STAGE_FALLBACK;
  copy_diagnostic(query->view.diagnostic, diagnostic);
}

static ql_status build_query_artifacts(
    ql_loop_proof_query *query, const ql_loop_analysis *left_analysis,
    const ql_loop_analysis *right_analysis, const ql_ir *left_ir,
    const ql_ir *right_ir, const loop_pair *pairs, size_t pair_count,
    uint32_t actual_scalar_path, uint32_t missing_candidate_ok,
    ql_error *error) {
  static const char induction_terminal[] =
      "(assert quodlibet_loop_induction_bad)\n";
  static const char summary_terminal[] =
      "(assert quodlibet_loop_summary_bad)\n";
  static const char domain_terminal[] = "(assert quodlibet_loop_domain)\n";
  loop_buffer prefix;
  loop_buffer base_terms;
  loop_buffer guard_terms;
  loop_buffer step_terms;
  loop_buffer exit_terms;
  loop_buffer summary_terms;
  size_t pair_index;
  uint32_t maximum_width = 1u;
  ql_status status;

  buffer_init(&prefix, &query->allocator);
  buffer_init(&base_terms, &query->allocator);
  buffer_init(&guard_terms, &query->allocator);
  buffer_init(&step_terms, &query->allocator);
  buffer_init(&exit_terms, &query->allocator);
  buffer_init(&summary_terms, &query->allocator);
  status = buffer_add(&prefix, "(set-logic QF_AUFBV)\n", error);
  if (status == QL_STATUS_OK && actual_scalar_path != 0u) {
    status = declare_shared_scalar_parameters(&prefix, left_ir, right_ir,
                                              &maximum_width, error);
  }
  if (status == QL_STATUS_OK) {
    status = buffer_add(&base_terms, "(or false", error);
  }
  if (status == QL_STATUS_OK) {
    status = buffer_add(&guard_terms, "(or false", error);
  }
  if (status == QL_STATUS_OK) {
    status = buffer_add(&step_terms, "(or false", error);
  }
  if (status == QL_STATUS_OK) {
    status = buffer_add(&exit_terms, "(or false", error);
  }
  if (status == QL_STATUS_OK) {
    status = buffer_add(&summary_terms, "(or false", error);
  }
  for (pair_index = 0u; pair_index < pair_count && status == QL_STATUS_OK;
       ++pair_index) {
    const ql_loop_view *left =
        ql_loop_analysis_loop_at(left_analysis, pairs[pair_index].left);
    const ql_loop_view *right =
        ql_loop_analysis_loop_at(right_analysis, pairs[pair_index].right);
    if (left == NULL || right == NULL) {
      ql_error_set(error, QL_STATUS_INTERNAL_ERROR,
                   "paired loop disappeared during SMT construction");
      status = QL_STATUS_INTERNAL_ERROR;
      break;
    }
    if (actual_scalar_path != 0u) {
      status = build_actual_scalar_loop_terms(
          query, &prefix, &base_terms, &guard_terms, &step_terms, &exit_terms,
          left_ir, right_ir, pairs[pair_index].left, pair_index, left, right,
          &maximum_width, missing_candidate_ok, error);
    } else {
      status = build_structural_loop_terms(&prefix, &base_terms, &guard_terms,
                                           &step_terms, &exit_terms, pair_index,
                                           left, &maximum_width, error);
    }
    if (status == QL_STATUS_OK && query->view.affine_summary_available != 0u) {
      status = build_summary_loop_terms(&prefix, &summary_terms, pair_index,
                                        left, right, error);
    }
  }
  if (status == QL_STATUS_OK) {
    status = buffer_add(&base_terms, ")", error);
  }
  if (status == QL_STATUS_OK) {
    status = buffer_add(&guard_terms, ")", error);
  }
  if (status == QL_STATUS_OK) {
    status = buffer_add(&step_terms, ")", error);
  }
  if (status == QL_STATUS_OK) {
    status = buffer_add(&exit_terms, ")", error);
  }
  if (status == QL_STATUS_OK) {
    status = buffer_add(&summary_terms, ")", error);
  }
  if (status == QL_STATUS_OK) {
    status = buffer_add(&prefix, "(define-fun quodlibet_loop_base_bad () Bool ",
                        error);
  }
  if (status == QL_STATUS_OK) {
    status = buffer_add_n(&prefix, base_terms.data, base_terms.size, error);
  }
  if (status == QL_STATUS_OK) {
    status = buffer_add(
        &prefix, ")\n(define-fun quodlibet_loop_guard_bad () Bool ", error);
  }
  if (status == QL_STATUS_OK) {
    status = buffer_add_n(&prefix, guard_terms.data, guard_terms.size, error);
  }
  if (status == QL_STATUS_OK) {
    status = buffer_add(
        &prefix, ")\n(define-fun quodlibet_loop_step_bad () Bool ", error);
  }
  if (status == QL_STATUS_OK) {
    status = buffer_add_n(&prefix, step_terms.data, step_terms.size, error);
  }
  if (status == QL_STATUS_OK) {
    status = buffer_add(
        &prefix, ")\n(define-fun quodlibet_loop_exit_bad () Bool ", error);
  }
  if (status == QL_STATUS_OK) {
    status = buffer_add_n(&prefix, exit_terms.data, exit_terms.size, error);
  }
  if (status == QL_STATUS_OK) {
    status = buffer_add(&prefix,
                        ")\n(define-fun quodlibet_loop_induction_bad () Bool "
                        "(or quodlibet_loop_base_bad quodlibet_loop_guard_bad "
                        "quodlibet_loop_step_bad quodlibet_loop_exit_bad))\n",
                        error);
  }
  if (status == QL_STATUS_OK && query->view.affine_summary_available != 0u) {
    status = buffer_add(
        &prefix, "(define-fun quodlibet_loop_summary_bad () Bool ", error);
    if (status == QL_STATUS_OK) {
      status =
          buffer_add_n(&prefix, summary_terms.data, summary_terms.size, error);
    }
    if (status == QL_STATUS_OK) {
      status = buffer_add(&prefix, ")\n", error);
    }
  }
  if (status == QL_STATUS_OK) {
    status = buffer_add(
        &prefix, "(define-fun quodlibet_loop_domain () Bool true)\n", error);
  }
  if (status == QL_STATUS_OK) {
    status = digest_bound_obligation(
        &query->allocator, &query->view.canonical_digest, &prefix, "Base",
        &base_terms, &query->view.base_obligation_digest, error);
  }
  if (status == QL_STATUS_OK) {
    status = digest_bound_obligation(
        &query->allocator, &query->view.canonical_digest, &prefix,
        "GuardAlignment", &guard_terms, &query->view.guard_obligation_digest,
        error);
  }
  if (status == QL_STATUS_OK) {
    status = digest_bound_obligation(
        &query->allocator, &query->view.canonical_digest, &prefix, "Step",
        &step_terms, &query->view.step_obligation_digest, error);
  }
  if (status == QL_STATUS_OK) {
    status = digest_bound_obligation(
        &query->allocator, &query->view.canonical_digest, &prefix, "Exit",
        &exit_terms, &query->view.exit_obligation_digest, error);
  }
  if (status == QL_STATUS_OK) {
    status = digest_bound_obligation(
        &query->allocator, &query->view.canonical_digest, &prefix, "Summary",
        &summary_terms, &query->view.summary_obligation_digest, error);
  }
  if (status == QL_STATUS_OK) {
    status = ql_artifact_create(&query->allocator, QL_ARTIFACT_KIND_SMTLIB2,
                                QL_SMTLIB2_SCHEMA_VERSION, prefix.data,
                                prefix.size, &query->prefix, error);
  }
  if (status == QL_STATUS_OK) {
    status = ql_artifact_create(&query->allocator, QL_ARTIFACT_KIND_SMTLIB2,
                                QL_SMTLIB2_SCHEMA_VERSION, induction_terminal,
                                sizeof(induction_terminal) - 1u,
                                &query->induction, error);
  }
  if (status == QL_STATUS_OK && query->view.affine_summary_available != 0u) {
    status = ql_artifact_create(&query->allocator, QL_ARTIFACT_KIND_SMTLIB2,
                                QL_SMTLIB2_SCHEMA_VERSION, summary_terminal,
                                sizeof(summary_terminal) - 1u, &query->summary,
                                error);
  }
  if (status == QL_STATUS_OK) {
    status = ql_artifact_create(
        &query->allocator, QL_ARTIFACT_KIND_SMTLIB2, QL_SMTLIB2_SCHEMA_VERSION,
        domain_terminal, sizeof(domain_terminal) - 1u, &query->domain, error);
  }
  if (status == QL_STATUS_OK) {
    status = artifact_digest(query->prefix, &query->view.prefix_digest, error);
  }
  if (status == QL_STATUS_OK) {
    status = artifact_digest(query->induction,
                             &query->view.induction_terminal_digest, error);
  }
  if (status == QL_STATUS_OK && query->summary != NULL) {
    status = artifact_digest(query->summary,
                             &query->view.summary_terminal_digest, error);
  }
  if (status == QL_STATUS_OK) {
    status = artifact_digest(query->domain, &query->view.domain_terminal_digest,
                             error);
  }
  if (status == QL_STATUS_OK) {
    query->view.maximum_bv_width = maximum_width;
  }
  buffer_dispose(&summary_terms);
  buffer_dispose(&exit_terms);
  buffer_dispose(&step_terms);
  buffer_dispose(&guard_terms);
  buffer_dispose(&base_terms);
  buffer_dispose(&prefix);
  return status;
}

static ql_status build_reflexivity_artifacts(
    ql_loop_proof_query *query, const ql_digest *left_ir_digest,
    const ql_digest *right_ir_digest, ql_error *error) {
  static const char terminal[] =
      "(assert quodlibet_exact_reflexivity_bad)\n";
  loop_buffer prefix;
  loop_buffer obligation;
  char left_hex[QL_DIGEST_HEX_SIZE];
  char right_hex[QL_DIGEST_HEX_SIZE];
  ql_status status;

  buffer_init(&prefix, &query->allocator);
  buffer_init(&obligation, &query->allocator);
  if (!ql_digest_equal(left_ir_digest, right_ir_digest)) {
    ql_error_set(error, QL_STATUS_INVALID_ARGUMENT,
                 "exact reflexivity requires identical IR artifact digests");
    return QL_STATUS_INVALID_ARGUMENT;
  }
  ql_digest_hex(left_ir_digest, left_hex);
  ql_digest_hex(right_ir_digest, right_hex);
  status = buffer_add(&prefix, "(set-logic QF_BV)\n", error);
  if (status == QL_STATUS_OK) {
    status = buffer_addf(&obligation, error, "(not (= #x%s #x%s))", left_hex,
                         right_hex);
  }
  if (status == QL_STATUS_OK) {
    status = buffer_addf(
        &prefix, error,
        "(define-fun quodlibet_left_ir_digest () (_ BitVec 256) #x%s)\n"
        "(define-fun quodlibet_right_ir_digest () (_ BitVec 256) #x%s)\n"
        "(define-fun quodlibet_exact_reflexivity_bad () Bool "
        "(not (= quodlibet_left_ir_digest quodlibet_right_ir_digest)))\n",
        left_hex, right_hex);
  }
  if (status == QL_STATUS_OK) {
    status = digest_bound_obligation(
        &query->allocator, &query->view.canonical_digest, &prefix,
        "ExactWholeIrReflexivity", &obligation,
        &query->view.reflexivity_obligation_digest, error);
  }
  if (status == QL_STATUS_OK) {
    status = ql_artifact_create(&query->allocator, QL_ARTIFACT_KIND_SMTLIB2,
                                QL_SMTLIB2_SCHEMA_VERSION, prefix.data,
                                prefix.size, &query->prefix, error);
  }
  if (status == QL_STATUS_OK) {
    status = ql_artifact_create(&query->allocator, QL_ARTIFACT_KIND_SMTLIB2,
                                QL_SMTLIB2_SCHEMA_VERSION, terminal,
                                sizeof(terminal) - 1u, &query->reflexivity,
                                error);
  }
  if (status == QL_STATUS_OK) {
    status = artifact_digest(query->prefix, &query->view.prefix_digest, error);
  }
  if (status == QL_STATUS_OK) {
    status = artifact_digest(query->reflexivity,
                             &query->view.reflexivity_terminal_digest, error);
  }
  if (status == QL_STATUS_OK) {
    query->view.logic = QL_SOLVER_LOGIC_QF_BV;
    query->view.maximum_bv_width = 256u;
  }
  buffer_dispose(&obligation);
  buffer_dispose(&prefix);
  return status;
}

void ql_loop_proof_options_init(ql_loop_proof_options_v1 *options) {
  if (options == NULL) {
    return;
  }
  memset(options, 0, sizeof(*options));
  options->struct_size = sizeof(*options);
  options->precondition_is_true = 1u;
  options->contract_binding_match = 0u;
}

void ql_loop_proof_query_destroy(ql_loop_proof_query *query) {
  ql_allocator allocator;

  if (query == NULL) {
    return;
  }
  allocator = query->allocator;
  ql_artifact_release(query->prefix);
  ql_artifact_release(query->induction);
  ql_artifact_release(query->reflexivity);
  ql_artifact_release(query->summary);
  ql_artifact_release(query->domain);
  allocator.deallocate(allocator.user_data, query->candidates);
  allocator.deallocate(allocator.user_data, query);
}

ql_status ql_loop_proof_query_build(const ql_allocator *allocator,
                                    const ql_ir *left_ir, const ql_ir *right_ir,
                                    const ql_loop_proof_options_v1 *options,
                                    ql_loop_proof_query **output,
                                    ql_error *error) {
  const ql_allocator *selected = select_allocator(allocator);
  ql_loop_proof_options_v1 selected_options;
  ql_loop_proof_query *query = NULL;
  ql_loop_analysis *left_analysis = NULL;
  ql_loop_analysis *right_analysis = NULL;
  const ql_loop_analysis_view *left_analysis_view;
  const ql_loop_analysis_view *right_analysis_view;
  ql_ir_view_v1 left_ir_view;
  ql_ir_view_v1 right_ir_view;
  loop_pair *pairs = NULL;
  size_t pair_count = 0u;
  loop_scan whole_left;
  loop_scan whole_right;
  uint32_t all_scalar = 1u;
  uint32_t any_opaque_state = 0u;
  uint32_t actual_scalar_path = 0u;
  uint32_t exact_shared_path = 0u;
  uint32_t semantic_fallback = 0u;
  uint32_t deferred_candidate_fallback = 0u;
  uint32_t concrete_domain_witness = 0u;
  ql_digest concrete_domain_witness_digest;
  char concrete_domain_witness_reason[QL_ERROR_MESSAGE_CAPACITY];
  char exact_fallback_diagnostic[QL_ERROR_MESSAGE_CAPACITY];
  ql_loop_proof_unsupported_reason fallback_reason =
      QL_LOOP_PROOF_UNSUPPORTED_NONE;
  const char *fallback_diagnostic = NULL;
  loop_buffer canonical;
  uint64_t total_started;
  uint64_t stage_started;
  size_t pair_index;
  ql_status status;

  if (output == NULL || left_ir == NULL || right_ir == NULL) {
    ql_error_set(error, QL_STATUS_INVALID_ARGUMENT,
                 "both IR functions and a loop-proof query output are "
                 "required");
    return QL_STATUS_INVALID_ARGUMENT;
  }
  *output = NULL;
  if (!ql_allocator_is_valid(selected)) {
    ql_error_set(error, QL_STATUS_INVALID_ARGUMENT, "invalid allocator");
    return QL_STATUS_INVALID_ARGUMENT;
  }
  ql_loop_proof_options_init(&selected_options);
  if (options != NULL) {
    if (options->struct_size != 0u && options->struct_size < sizeof(*options)) {
      ql_error_set(error, QL_STATUS_ABI_MISMATCH,
                   "loop-proof options structure is too small");
      return QL_STATUS_ABI_MISMATCH;
    }
    if (options->precondition_is_true > 1u ||
        options->contract_binding_match > 1u) {
      ql_error_set(error, QL_STATUS_INVALID_ARGUMENT,
                   "loop-proof option flags must be zero or one");
      return QL_STATUS_INVALID_ARGUMENT;
    }
    selected_options = *options;
  }
  total_started = now_ns();
  query = selected->allocate(selected->user_data, sizeof(*query));
  if (query == NULL) {
    ql_error_set(error, QL_STATUS_OUT_OF_MEMORY, NULL);
    return QL_STATUS_OUT_OF_MEMORY;
  }
  memset(query, 0, sizeof(*query));
  query->allocator = *selected;
  query->view.struct_size = sizeof(query->view);
  query->view.schema_version = QL_LOOP_PROOF_QUERY_SCHEMA_VERSION;
  query->view.disposition = QL_LOOP_PROOF_NOT_APPLICABLE;
  query->view.strategy = QL_LOOP_PROOF_STRATEGY_NONE;
  query->view.logic = QL_SOLVER_LOGIC_QF_AUFBV;
  query->view.maximum_bv_width = 1u;
  query->view.requires_contract_binding_match = 1u;
  query->view.domain_is_abstract_true = 1u;
  query->view.requires_concrete_domain_check = 1u;
  query->view.chc_pdr_available = 0u;
  query->view.candidate_sat_is_counterexample = 0u;
  query->view.metrics.struct_size = sizeof(query->view.metrics);
  query->view.metrics.schema_version = QL_LOOP_PROOF_QUERY_SCHEMA_VERSION;
  query->view.metrics.induction_answer = QL_SMT_PRODUCT_ANSWER_NOT_QUERIED;
  query->view.metrics.summary_answer = QL_SMT_PRODUCT_ANSWER_NOT_QUERIED;
  query->view.metrics.domain_answer = QL_SMT_PRODUCT_ANSWER_NOT_QUERIED;
  query->view.metrics.reflexivity_answer =
      QL_SMT_PRODUCT_ANSWER_NOT_QUERIED;
  memset(&concrete_domain_witness_digest, 0,
         sizeof(concrete_domain_witness_digest));
  copy_diagnostic(concrete_domain_witness_reason,
                  "the concrete witness was not attempted");
  exact_fallback_diagnostic[0] = '\0';
  buffer_init(&canonical, selected);

  query->view.metrics.stage_reached |= QL_LOOP_STAGE_DISCOVER;
  stage_started = now_ns();
  status = get_ir_view(left_ir, &left_ir_view, error);
  if (status == QL_STATUS_OK) {
    status = get_ir_view(right_ir, &right_ir_view, error);
  }
  if (status == QL_STATUS_OK) {
    status = ql_loop_analysis_create(selected, left_ir, &left_analysis, error);
  }
  if (status == QL_STATUS_OK) {
    status =
        ql_loop_analysis_create(selected, right_ir, &right_analysis, error);
  }
  query->view.metrics.discover_ns = elapsed_ns(stage_started);
  if (status != QL_STATUS_OK) {
    goto failure;
  }
  left_analysis_view = ql_loop_analysis_get_view(left_analysis);
  right_analysis_view = ql_loop_analysis_get_view(right_analysis);
  if (left_analysis_view == NULL || right_analysis_view == NULL) {
    ql_error_set(error, QL_STATUS_INTERNAL_ERROR,
                 "loop analyzer returned no analysis view");
    status = QL_STATUS_INTERNAL_ERROR;
    goto failure;
  }
  query->view.metrics.left_loop_count = left_analysis_view->loop_count;
  query->view.metrics.right_loop_count = right_analysis_view->loop_count;
  if (left_analysis_view->has_actual_cycle == 0u &&
      right_analysis_view->has_actual_cycle == 0u) {
    query->view.unsupported_reason = QL_LOOP_PROOF_UNSUPPORTED_NOT_CYCLIC;
    copy_diagnostic(query->view.diagnostic,
                    "neither IR function contains an actual CFG cycle");
    goto success;
  }
  if (left_ir_view.cfg_kind != QL_IR_CFG_CYCLIC ||
      right_ir_view.cfg_kind != QL_IR_CFG_CYCLIC ||
      left_analysis_view->has_actual_cycle == 0u ||
      right_analysis_view->has_actual_cycle == 0u) {
    semantic_fallback = 1u;
    fallback_reason = QL_LOOP_PROOF_UNSUPPORTED_LOOP_COUNT_MISMATCH;
    fallback_diagnostic =
        "only one IR function contains an actual loop, so relational "
        "pairing cannot cover both sides";
    goto finish_dispatch;
  }
  if (left_analysis_view->has_unclassified_cycle != 0u ||
      right_analysis_view->has_unclassified_cycle != 0u) {
    semantic_fallback = 1u;
    fallback_reason = QL_LOOP_PROOF_UNSUPPORTED_UNCLASSIFIED_CYCLE;
    fallback_diagnostic = "a CFG cycle is not a reducible dominance backedge";
    goto finish_dispatch;
  }
  if (left_analysis_view->loop_count != right_analysis_view->loop_count) {
    semantic_fallback = 1u;
    fallback_reason = QL_LOOP_PROOF_UNSUPPORTED_LOOP_COUNT_MISMATCH;
    fallback_diagnostic = "the two cyclic IR functions have different loop "
                          "counts";
    goto finish_dispatch;
  }

  query->view.metrics.stage_reached |= QL_LOOP_STAGE_CANONICALIZE;
  stage_started = now_ns();
  query->view.self_pair = ql_digest_equal(&left_ir_view.artifact_digest,
                                          &right_ir_view.artifact_digest);
  status = ir_structurally_equal(left_ir, right_ir,
                                 &query->view.ir_structural_match, error);
  if (status == QL_STATUS_OK && query->view.self_pair != 0u &&
      query->view.ir_structural_match != 0u &&
      selected_options.precondition_is_true != 0u) {
    status = build_concrete_domain_witness(
        selected, left_ir, &concrete_domain_witness,
        &concrete_domain_witness_digest, concrete_domain_witness_reason,
        error);
  }
  query->view.metrics.concrete_domain_witness = concrete_domain_witness;
  query->view.metrics.domain_witness_digest =
      concrete_domain_witness_digest;
  query->view.metrics.canonicalize_ns = elapsed_ns(stage_started);
  if (status != QL_STATUS_OK) {
    goto failure;
  }

  query->view.metrics.stage_reached |= QL_LOOP_STAGE_PAIRING;
  stage_started = now_ns();
  status = pair_loops(selected, left_analysis, right_analysis, &pairs,
                      &pair_count, error);
  query->view.metrics.pairing_ns = elapsed_ns(stage_started);
  if (status != QL_STATUS_OK) {
    goto failure;
  }
  query->view.metrics.paired_loop_count = pair_count;
  query->view.all_loops_paired = pair_count == left_analysis_view->loop_count &&
                                 pair_count == right_analysis_view->loop_count;
  if (query->view.all_loops_paired == 0u) {
    semantic_fallback = 1u;
    fallback_reason = QL_LOOP_PROOF_UNSUPPORTED_PAIRING;
    fallback_diagnostic =
        "no one-to-one pairing covers every canonical loop shape";
    goto finish_dispatch;
  }

  query->view.metrics.stage_reached |= QL_LOOP_STAGE_INVARIANT;
  stage_started = now_ns();
  for (pair_index = 0u; pair_index < pair_count; ++pair_index) {
    const ql_loop_view *left =
        ql_loop_analysis_loop_at(left_analysis, pairs[pair_index].left);
    const ql_loop_view *right =
        ql_loop_analysis_loop_at(right_analysis, pairs[pair_index].right);
    loop_scan left_scan;
    loop_scan right_scan;
    size_t phi_index;
    uint32_t allow_opaque_state;
    uint64_t non_ub_effects;

    if (left == NULL || right == NULL) {
      ql_error_set(error, QL_STATUS_INTERNAL_ERROR,
                   "paired loop disappeared during validation");
      status = QL_STATUS_INTERNAL_ERROR;
      goto failure;
    }
    if (left->is_reducible_single_entry == 0u ||
        right->is_reducible_single_entry == 0u) {
      semantic_fallback = 1u;
      fallback_reason = QL_LOOP_PROOF_UNSUPPORTED_IRREDUCIBLE_OR_MULTI_ENTRY;
      fallback_diagnostic =
          "loop fast path requires reducible single-entry natural loops";
      break;
    }
    if (left->parent != QL_LOOP_INVALID_INDEX ||
        right->parent != QL_LOOP_INVALID_INDEX || left->nesting_depth != 0u ||
        right->nesting_depth != 0u) {
      semantic_fallback = 1u;
      fallback_reason = QL_LOOP_PROOF_UNSUPPORTED_NESTED_LOOP;
      fallback_diagnostic =
          "nested loops are outside the first relational induction fast path";
      break;
    }
    if (left->preheader == QL_IR_INVALID_BLOCK_ID ||
        right->preheader == QL_IR_INVALID_BLOCK_ID || left->latch_count == 0u ||
        right->latch_count == 0u || left->exit_count == 0u ||
        right->exit_count == 0u || left->guard.position == QL_LOOP_GUARD_NONE ||
        right->guard.position == QL_LOOP_GUARD_NONE ||
        left->guard.position == QL_LOOP_GUARD_AMBIGUOUS ||
        right->guard.position == QL_LOOP_GUARD_AMBIGUOUS ||
        left->guard.condition == QL_IR_INVALID_VALUE_ID ||
        right->guard.condition == QL_IR_INVALID_VALUE_ID) {
      semantic_fallback = 1u;
      fallback_reason = QL_LOOP_PROOF_UNSUPPORTED_GUARD;
      fallback_diagnostic =
          "loop entry or continue/exit guard is not canonical and unambiguous";
      break;
    }
    status = scan_loop(left_ir, left, &left_scan, error);
    if (status == QL_STATUS_OK) {
      status = scan_loop(right_ir, right, &right_scan, error);
    }
    if (status != QL_STATUS_OK) {
      goto failure;
    }
    if (left_scan.has_ub != 0u || right_scan.has_ub != 0u) {
      query->view.has_ub_guard_or_terminator = 1u;
    }
    allow_opaque_state =
        query->view.self_pair != 0u && query->view.ir_structural_match != 0u;
    for (phi_index = 0u; phi_index < left->phi_count; ++phi_index) {
      const ql_loop_phi_view *left_phi = &left->phis[phi_index];
      const ql_loop_phi_view *right_phi = &right->phis[phi_index];
      if (left_phi->entry_block == QL_IR_INVALID_BLOCK_ID ||
          right_phi->entry_block == QL_IR_INVALID_BLOCK_ID ||
          left_phi->entry_value == QL_IR_INVALID_VALUE_ID ||
          right_phi->entry_value == QL_IR_INVALID_VALUE_ID ||
          left_phi->type_kind != right_phi->type_kind ||
          left_phi->bit_width != right_phi->bit_width ||
          !phi_sort_supported(left_phi, allow_opaque_state) ||
          !phi_sort_supported(right_phi, allow_opaque_state)) {
        semantic_fallback = 1u;
        fallback_reason = QL_LOOP_PROOF_UNSUPPORTED_NONSCALAR_STATE;
        fallback_diagnostic =
            "header PHI state is not an exactly paired supported state sort";
        break;
      }
      if (left_phi->type_kind != QL_IR_TYPE_BOOL &&
          left_phi->type_kind != QL_IR_TYPE_BIT_VECTOR) {
        all_scalar = 0u;
        any_opaque_state = 1u;
      }
      if (left_phi->type_kind == QL_IR_TYPE_POINTER) {
        left_scan.has_pointer = 1u;
        right_scan.has_pointer = 1u;
      } else if (left_phi->type_kind == QL_IR_TYPE_MEMORY) {
        left_scan.has_memory = 1u;
        right_scan.has_memory = 1u;
      } else if (left_phi->type_kind == QL_IR_TYPE_EVENT_TRACE) {
        left_scan.has_event_trace = 1u;
        right_scan.has_event_trace = 1u;
      }
    }
    if (semantic_fallback != 0u) {
      break;
    }
    non_ub_effects = (left_scan.effects | right_scan.effects) &
                     ~((uint64_t)QL_IR_EFFECT_UNDEFINED_BEHAVIOR);
    if ((left_scan.has_memory != 0u || right_scan.has_memory != 0u ||
         left_scan.has_event_trace != 0u || right_scan.has_event_trace != 0u ||
         left_scan.has_pointer != 0u || right_scan.has_pointer != 0u ||
         non_ub_effects != 0u) &&
        allow_opaque_state == 0u) {
      semantic_fallback = 1u;
      if (left_scan.has_call != 0u || right_scan.has_call != 0u) {
        fallback_reason = QL_LOOP_PROOF_UNSUPPORTED_CALL;
        fallback_diagnostic =
            "calls require the exact self-pair opaque-state path";
      } else if (left_scan.has_memory != 0u || right_scan.has_memory != 0u) {
        fallback_reason = QL_LOOP_PROOF_UNSUPPORTED_MEMORY_STATE;
        fallback_diagnostic =
            "memory loops are outside scalar relational invariant synthesis";
      } else if (left_scan.has_event_trace != 0u ||
                 right_scan.has_event_trace != 0u) {
        fallback_reason = QL_LOOP_PROOF_UNSUPPORTED_EVENT_TRACE;
        fallback_diagnostic =
            "event traces require the exact self-pair opaque-state path";
      } else {
        fallback_reason = QL_LOOP_PROOF_UNSUPPORTED_EFFECT;
        fallback_diagnostic =
            "effectful loops are outside scalar relational invariant synthesis";
      }
      break;
    }
    if (allow_opaque_state != 0u && non_ub_effects != 0u) {
      const uint64_t call_or_observed =
          non_ub_effects &
          ((uint64_t)QL_IR_EFFECT_CALL | (uint64_t)QL_IR_EFFECT_VOLATILE |
           (uint64_t)QL_IR_EFFECT_ATOMIC | (uint64_t)QL_IR_EFFECT_IO);
      if (((non_ub_effects & (uint64_t)QL_IR_EFFECT_MEMORY) != 0u &&
           (left_scan.has_memory == 0u || right_scan.has_memory == 0u)) ||
          (call_or_observed != 0u &&
           (left_scan.has_memory == 0u || right_scan.has_memory == 0u ||
            left_scan.has_event_trace == 0u ||
            right_scan.has_event_trace == 0u))) {
        semantic_fallback = 1u;
        fallback_reason = QL_LOOP_PROOF_UNSUPPORTED_EFFECT;
        fallback_diagnostic =
            "an effect is not carried by the exact paired memory/event state";
        break;
      }
      any_opaque_state = 1u;
      all_scalar = 0u;
    }
    for (phi_index = 0u; phi_index < left->phi_count && status == QL_STATUS_OK;
         ++phi_index) {
      status = generate_phi_candidates(
          query, left_ir, right_ir, pairs[pair_index].left,
          pairs[pair_index].right, phi_index, &left->phis[phi_index],
          &right->phis[phi_index], error);
    }
    if (status != QL_STATUS_OK) {
      goto failure;
    }
  }
  query->view.metrics.invariant_ns = elapsed_ns(stage_started);
  if (semantic_fallback != 0u) {
    goto finish_dispatch;
  }
  query->view.metrics.invariant_candidate_count = query->view.candidate_count;
  status = scan_whole_ir(left_ir, &whole_left, error);
  if (status == QL_STATUS_OK) {
    status = scan_whole_ir(right_ir, &whole_right, error);
  }
  if (status != QL_STATUS_OK) {
    goto failure;
  }
  if (whole_left.has_ub != 0u || whole_right.has_ub != 0u) {
    query->view.has_ub_guard_or_terminator = 1u;
  }
  if (query->view.self_pair != 0u &&
      query->view.ir_structural_match != 0u &&
      concrete_domain_witness != 0u &&
      (whole_left.has_assume != 0u || whole_right.has_assume != 0u ||
       whole_left.has_ub != 0u || whole_right.has_ub != 0u ||
       whole_left.effects != 0u || whole_right.effects != 0u ||
       whole_left.has_memory != 0u || whole_right.has_memory != 0u ||
       whole_left.has_event_trace != 0u || whole_right.has_event_trace != 0u ||
       whole_left.has_pointer != 0u || whole_right.has_pointer != 0u)) {
    semantic_fallback = 1u;
    fallback_reason = QL_LOOP_PROOF_UNSUPPORTED_EFFECT;
    fallback_diagnostic =
        "exact whole-IR reflexivity is required to cover assumptions, UB, "
        "memory, trace, pointers, or effects";
    goto finish_dispatch;
  }
  actual_scalar_path = all_scalar;
  if (actual_scalar_path != 0u) {
    uint32_t missing_candidate = 0u;
    for (pair_index = 0u; pair_index < pair_count; ++pair_index) {
      const ql_loop_view *left =
          ql_loop_analysis_loop_at(left_analysis, pairs[pair_index].left);
      size_t phi_index;
      if (left == NULL) {
        ql_error_set(error, QL_STATUS_INTERNAL_ERROR,
                     "paired loop disappeared before query construction");
        status = QL_STATUS_INTERNAL_ERROR;
        goto failure;
      }
      for (phi_index = 0u; phi_index < left->phi_count; ++phi_index) {
        if (selected_candidate(query, pairs[pair_index].left, phi_index) ==
            NULL) {
          missing_candidate = 1u;
          break;
        }
      }
      if (missing_candidate != 0u) {
        break;
      }
    }
    if (missing_candidate != 0u) {
      if (query->view.self_pair != 0u &&
          query->view.ir_structural_match != 0u) {
        actual_scalar_path = 0u;
        exact_shared_path = 1u;
      } else {
        /* The fixed relation vocabulary cannot cover this pair, but the
           actual transition serialization does not depend on it. The
           remaining structural gates run as usual, and when they pass the
           prefix is built and kept for a CHC/PDR backend while the
           disposition still records the fallback. */
        deferred_candidate_fallback = 1u;
      }
    }
  }
  if (actual_scalar_path == 0u &&
      (query->view.self_pair == 0u || query->view.ir_structural_match == 0u)) {
    semantic_fallback = 1u;
    fallback_reason = QL_LOOP_PROOF_UNSUPPORTED_NONSCALAR_STATE;
    fallback_diagnostic =
        "non-scalar state is allowed only for an exact whole-IR self-pair";
    goto finish_dispatch;
  }
  if (actual_scalar_path == 0u) {
    exact_shared_path = 1u;
  }
  if (actual_scalar_path != 0u &&
      (whole_left.has_assume != 0u || whole_right.has_assume != 0u ||
       (whole_left.effects & ~((uint64_t)QL_IR_EFFECT_UNDEFINED_BEHAVIOR)) !=
           0u ||
       (whole_right.effects & ~((uint64_t)QL_IR_EFFECT_UNDEFINED_BEHAVIOR)) !=
           0u ||
       whole_left.has_memory != 0u || whole_right.has_memory != 0u ||
       whole_left.has_event_trace != 0u || whole_right.has_event_trace != 0u ||
       whole_left.has_pointer != 0u || whole_right.has_pointer != 0u)) {
    if (query->view.self_pair != 0u && query->view.ir_structural_match != 0u) {
      actual_scalar_path = 0u;
      exact_shared_path = 1u;
      any_opaque_state = 1u;
    } else {
      semantic_fallback = 1u;
      fallback_reason = QL_LOOP_PROOF_UNSUPPORTED_EFFECT;
      fallback_diagnostic =
          "actual relational transition does not cover whole-IR assumptions, "
          "memory, trace, or effects";
      goto finish_dispatch;
    }
  }
  if (actual_scalar_path != 0u && pair_count != 1u) {
    if (query->view.self_pair != 0u && query->view.ir_structural_match != 0u) {
      actual_scalar_path = 0u;
      exact_shared_path = 1u;
    } else {
      semantic_fallback = 1u;
      fallback_reason =
          QL_LOOP_PROOF_UNSUPPORTED_ENTRY_TRANSITION_EXIT_MISMATCH;
      fallback_diagnostic =
          "the actual scalar path currently closes one loop directly to the "
          "whole-program return";
      goto finish_dispatch;
    }
  }
  if (actual_scalar_path != 0u) {
    const ql_loop_view *left =
        ql_loop_analysis_loop_at(left_analysis, pairs[0].left);
    const ql_loop_view *right =
        ql_loop_analysis_loop_at(right_analysis, pairs[0].right);
    uint32_t left_shape = 0u;
    uint32_t right_shape = 0u;
    uint32_t exit_types_match = 0u;
    if (left == NULL || right == NULL) {
      ql_error_set(error, QL_STATUS_INTERNAL_ERROR,
                   "paired loop disappeared before actual-path validation");
      status = QL_STATUS_INTERNAL_ERROR;
      goto failure;
    }
    if (left->guard.position != QL_LOOP_GUARD_PRE_TEST ||
        right->guard.position != QL_LOOP_GUARD_PRE_TEST) {
      if (query->view.self_pair != 0u &&
          query->view.ir_structural_match != 0u) {
        actual_scalar_path = 0u;
        exact_shared_path = 1u;
      } else {
        semantic_fallback = 1u;
        fallback_reason = QL_LOOP_PROOF_UNSUPPORTED_GUARD;
        fallback_diagnostic =
            "actual scalar promotion currently requires PRE_TEST guards";
        goto finish_dispatch;
      }
    }
    if (actual_scalar_path != 0u) {
      status =
          actual_program_shape_supported(left_ir, left, &left_shape, error);
      if (status == QL_STATUS_OK) {
        status = actual_program_shape_supported(right_ir, right, &right_shape,
                                                error);
      }
      if (status == QL_STATUS_OK) {
        status = actual_exit_types_compatible(left_ir, left, right_ir, right,
                                              &exit_types_match, error);
      }
      if (status != QL_STATUS_OK) {
        goto failure;
      }
      if (left_shape == 0u || right_shape == 0u || exit_types_match == 0u) {
        if (query->view.self_pair != 0u &&
            query->view.ir_structural_match != 0u) {
          actual_scalar_path = 0u;
          exact_shared_path = 1u;
        } else {
          semantic_fallback = 1u;
          fallback_reason =
              QL_LOOP_PROOF_UNSUPPORTED_ENTRY_TRANSITION_EXIT_MISMATCH;
          fallback_diagnostic =
              exit_types_match == 0u
                  ? "left and right direct-return expressions do not share "
                    "one Bool or <=64-bit bit-vector sort"
                  : "actual scalar proof requires an unconditional entry "
                    "chain, no loop bypass, and the loop's sole exit as the "
                    "function's sole return";
          goto finish_dispatch;
        }
      }
    }
  }
  query->view.canonical_transition_match =
      actual_scalar_path != 0u || query->view.ir_structural_match != 0u;
  query->view.canonical_entry_exit_match =
      query->view.canonical_transition_match;
  query->view.structural_query_available = 1u;
  query->view.exact_self_pair_opaque_state = exact_shared_path != 0u &&
                                             query->view.self_pair != 0u &&
                                             any_opaque_state != 0u;

  /* A variable-iteration summary is not promoted until its common iteration
     count and actual exit observable are represented.  The one-step actual
     induction path above already handles fixed affine recurrences without
     unrolling; do not expose the older disconnected summary tautology. */
  query->view.metrics.summary_candidate_count =
      affine_summary_available(left_analysis, right_analysis, pairs, pair_count)
          ? pair_count
          : 0u;
  query->view.affine_summary_available = 0u;
  query->view.metrics.summary_ns = 0u;

  stage_started = now_ns();
  status = build_canonical_digest(
      left_ir, right_ir, left_analysis, right_analysis, pairs, pair_count,
      query->view.ir_structural_match, actual_scalar_path, &canonical,
      &query->view.canonical_digest, error);
  if (status == QL_STATUS_OK) {
    status = build_query_artifacts(query, left_analysis, right_analysis,
                                   left_ir, right_ir, pairs, pair_count,
                                   actual_scalar_path,
                                   deferred_candidate_fallback, error);
  }
  if (status == QL_STATUS_TYPE_MISMATCH && actual_scalar_path != 0u &&
      query->view.self_pair != 0u && query->view.ir_structural_match != 0u) {
    ql_error_clear(error);
    actual_scalar_path = 0u;
    exact_shared_path = 1u;
    buffer_dispose(&canonical);
    buffer_init(&canonical, selected);
    status = build_canonical_digest(
        left_ir, right_ir, left_analysis, right_analysis, pairs, pair_count,
        query->view.ir_structural_match, actual_scalar_path, &canonical,
        &query->view.canonical_digest, error);
    if (status == QL_STATUS_OK) {
      status = build_query_artifacts(query, left_analysis, right_analysis,
                                     left_ir, right_ir, pairs, pair_count,
                                     actual_scalar_path, 0u, error);
    }
  }
  query->view.metrics.query_build_ns = elapsed_ns(stage_started);
  if (status == QL_STATUS_TYPE_MISMATCH) {
    ql_error_clear(error);
    semantic_fallback = 1u;
    fallback_reason = QL_LOOP_PROOF_UNSUPPORTED_ENTRY_TRANSITION_EXIT_MISMATCH;
    fallback_diagnostic =
        "entry leaves, guard DAG, recurrence, or direct-return exit is outside "
        "the exact scalar relational encoding";
    goto finish_dispatch;
  }
  if (status != QL_STATUS_OK) {
    goto failure;
  }
  query->view.metrics.invariant_generated_count =
      deferred_candidate_fallback != 0u ? 0u : pair_count;

  query->view.nonvacuity_eligible =
      (actual_scalar_path != 0u ||
       (exact_shared_path != 0u && all_scalar != 0u)) &&
      selected_options.precondition_is_true != 0u &&
      whole_left.has_assume == 0u && whole_right.has_assume == 0u &&
      whole_left.has_ub == 0u && whole_right.has_ub == 0u &&
      whole_left.effects == 0u && whole_right.effects == 0u &&
      whole_left.has_memory == 0u && whole_right.has_memory == 0u &&
      whole_left.has_event_trace == 0u && whole_right.has_event_trace == 0u &&
      whole_left.has_pointer == 0u && whole_right.has_pointer == 0u &&
      whole_left.has_unsupported_type == 0u &&
      whole_right.has_unsupported_type == 0u;
  query->view.domain_is_abstract_true = query->view.nonvacuity_eligible == 0u;
  query->view.requires_concrete_domain_check =
      query->view.nonvacuity_eligible == 0u;
  query->view.promotion_eligible =
      query->view.nonvacuity_eligible != 0u &&
      selected_options.contract_binding_match != 0u &&
      query->view.has_ub_guard_or_terminator == 0u;
  query->promotion_gate_satisfied = query->view.promotion_eligible;
  if (deferred_candidate_fallback != 0u) {
    /* The prefix and its raw entry, guard, next, and exit definitions were
       built and are kept, but the fixed-vocabulary induction terminal has
       nothing selected to check, so the fast path still records the
       fallback. `chc_pdr_available` says the serialized transition system
       is there for a CHC/PDR backend to synthesize an invariant over;
       nonvacuity and UB flags above stay meaningful for that backend's own
       promotion gate. */
    mark_fallback(query, QL_LOOP_PROOF_UNSUPPORTED_AFFINE_SUMMARY,
                  "no Base-and-recurrence-preserving equality, offset, or "
                  "affine relation covers every header PHI; the serialized "
                  "transition prefix remains available for CHC/PDR");
    query->promotion_gate_satisfied = 0u;
    query->view.chc_pdr_available = 1u;
    goto success;
  }
  query->view.disposition = QL_LOOP_PROOF_QUERY_READY;
  query->view.strategy = QL_LOOP_PROOF_STRATEGY_STRUCTURAL_INDUCTION;
  if (query->view.promotion_eligible != 0u) {
    copy_diagnostic(
        query->view.diagnostic,
        actual_scalar_path != 0u
            ? "actual entry, guard, affine recurrence, and exit expressions "
              "are ready for relational induction"
            : "exact whole-IR scalar reflexivity is ready for shared "
              "transition congruence induction");
  } else if (query->view.self_pair != 0u &&
             query->view.ir_structural_match != 0u &&
             concrete_domain_witness == 0u) {
    (void)snprintf(query->view.diagnostic, sizeof(query->view.diagnostic),
                   "loop obligations are available, but exact self-pair "
                   "promotion has no inhabited-domain witness: %s",
                   concrete_domain_witness_reason);
  } else {
    copy_diagnostic(query->view.diagnostic,
                    "loop query is available, but promotion requires every "
                    "recorded binding and non-vacuity gate");
  }
  goto success;

finish_dispatch:
  if (semantic_fallback != 0u && query->view.self_pair != 0u &&
      query->view.ir_structural_match != 0u &&
      query->view.all_loops_paired != 0u &&
      concrete_domain_witness != 0u &&
      selected_options.precondition_is_true != 0u &&
      selected_options.contract_binding_match != 0u) {
    buffer_dispose(&canonical);
    buffer_init(&canonical, selected);
    stage_started = now_ns();
    status = build_canonical_digest(
        left_ir, right_ir, left_analysis, right_analysis, pairs, pair_count,
        query->view.ir_structural_match, 0u, &canonical,
        &query->view.canonical_digest, error);
    if (status == QL_STATUS_OK) {
      status = build_reflexivity_artifacts(
          query, &left_ir_view.artifact_digest, &right_ir_view.artifact_digest,
          error);
    }
    query->view.metrics.query_build_ns += elapsed_ns(stage_started);
    if (status != QL_STATUS_OK) {
      goto failure;
    }
    query->view.canonical_transition_match = 1u;
    query->view.canonical_entry_exit_match = 1u;
    query->view.structural_query_available = 1u;
    query->view.nonvacuity_eligible = 1u;
    query->view.domain_is_abstract_true = 0u;
    query->view.requires_concrete_domain_check = 0u;
    query->view.promotion_eligible = 1u;
    query->promotion_gate_satisfied = 1u;
    query->view.disposition = QL_LOOP_PROOF_QUERY_READY;
    query->view.strategy = QL_LOOP_PROOF_STRATEGY_EXACT_REFLEXIVITY;
    query->view.unsupported_reason = QL_LOOP_PROOF_UNSUPPORTED_NONE;
    query->view.chc_pdr_reached = 0u;
    query->view.metrics.fallback_reached_count = 0u;
    query->view.metrics.stage_reached &= ~QL_LOOP_STAGE_FALLBACK;
    copy_diagnostic(
        query->view.diagnostic,
        "exact whole-IR identity and a concrete defined execution are ready "
        "for reflexivity; no loop invariant is claimed");
    goto success;
  }
  if (semantic_fallback != 0u) {
    if (query->view.self_pair != 0u &&
        query->view.ir_structural_match != 0u &&
        concrete_domain_witness == 0u) {
      (void)snprintf(exact_fallback_diagnostic,
                     sizeof(exact_fallback_diagnostic), "%s; exact concrete "
                     "domain witness failed: %s",
                     fallback_diagnostic != NULL ? fallback_diagnostic
                                                 : "loop fallback was reached",
                     concrete_domain_witness_reason);
      fallback_diagnostic = exact_fallback_diagnostic;
    }
    mark_fallback(query, fallback_reason, fallback_diagnostic);
  }

success:
  query->view.metrics.total_ns = elapsed_ns(total_started);
  selected->deallocate(selected->user_data, pairs);
  ql_loop_analysis_destroy(left_analysis);
  ql_loop_analysis_destroy(right_analysis);
  buffer_dispose(&canonical);
  *output = query;
  ql_error_clear(error);
  return QL_STATUS_OK;

failure:
  selected->deallocate(selected->user_data, pairs);
  ql_loop_analysis_destroy(left_analysis);
  ql_loop_analysis_destroy(right_analysis);
  buffer_dispose(&canonical);
  ql_loop_proof_query_destroy(query);
  return status;
}

ql_status ql_loop_proof_query_get_view(const ql_loop_proof_query *query,
                                       ql_loop_proof_query_view_v1 *view,
                                       ql_error *error) {
  size_t caller_size;

  if (query == NULL || view == NULL) {
    ql_error_set(error, QL_STATUS_INVALID_ARGUMENT,
                 "loop-proof query and view are required");
    return QL_STATUS_INVALID_ARGUMENT;
  }
  caller_size = view->struct_size;
  if (caller_size != 0u && caller_size < sizeof(*view)) {
    ql_error_set(error, QL_STATUS_ABI_MISMATCH,
                 "loop-proof query view structure is too small");
    return QL_STATUS_ABI_MISMATCH;
  }
  *view = query->view;
  ql_error_clear(error);
  return QL_STATUS_OK;
}

ql_status
ql_loop_proof_query_candidate_at(const ql_loop_proof_query *query, size_t index,
                                 ql_loop_relation_candidate_v1 *candidate,
                                 ql_error *error) {
  size_t caller_size;

  if (query == NULL || candidate == NULL) {
    ql_error_set(error, QL_STATUS_INVALID_ARGUMENT,
                 "loop-proof query and candidate output are required");
    return QL_STATUS_INVALID_ARGUMENT;
  }
  if (index >= query->view.candidate_count) {
    ql_error_set(error, QL_STATUS_NOT_FOUND,
                 "loop-proof query has no candidate %zu", index);
    return QL_STATUS_NOT_FOUND;
  }
  caller_size = candidate->struct_size;
  if (caller_size != 0u && caller_size < sizeof(*candidate)) {
    ql_error_set(error, QL_STATUS_ABI_MISMATCH,
                 "loop relation candidate structure is too small");
    return QL_STATUS_ABI_MISMATCH;
  }
  *candidate = query->candidates[index];
  ql_error_clear(error);
  return QL_STATUS_OK;
}

const ql_artifact *
ql_loop_proof_query_prefix_artifact(const ql_loop_proof_query *query) {
  return query == NULL ? NULL : query->prefix;
}

const ql_artifact *
ql_loop_proof_query_induction_artifact(const ql_loop_proof_query *query) {
  return query == NULL ? NULL : query->induction;
}

const ql_artifact *
ql_loop_proof_query_reflexivity_artifact(const ql_loop_proof_query *query) {
  return query == NULL ? NULL : query->reflexivity;
}

const ql_artifact *
ql_loop_proof_query_summary_artifact(const ql_loop_proof_query *query) {
  if (query == NULL ||
      query->view.metrics.induction_answer == QL_SMT_PRODUCT_ANSWER_UNKNOWN) {
    return NULL;
  }
  return query->summary;
}

const ql_artifact *
ql_loop_proof_query_domain_artifact(const ql_loop_proof_query *query) {
  if (query == NULL || query->view.requires_concrete_domain_check != 0u) {
    return NULL;
  }
  return query->domain;
}

ql_status ql_loop_proof_query_record_check(
    ql_loop_proof_query *query, ql_loop_proof_check_target target,
    ql_solver_check_kind answer, ql_solver_unknown_reason unknown_reason,
    uint64_t elapsed, ql_error *error) {
  ql_smt_product_answer recorded;

  if (query == NULL) {
    ql_error_set(error, QL_STATUS_INVALID_ARGUMENT,
                 "loop-proof query is required");
    return QL_STATUS_INVALID_ARGUMENT;
  }
  if (answer != QL_SOLVER_CHECK_SAT && answer != QL_SOLVER_CHECK_UNSAT &&
      answer != QL_SOLVER_CHECK_UNKNOWN) {
    ql_error_set(error, QL_STATUS_INVALID_ARGUMENT,
                 "loop-proof check answer is invalid");
    return QL_STATUS_INVALID_ARGUMENT;
  }
  if (answer != QL_SOLVER_CHECK_UNKNOWN &&
      unknown_reason != QL_SOLVER_UNKNOWN_NONE) {
    ql_error_set(error, QL_STATUS_INVALID_ARGUMENT,
                 "a decided loop-proof check cannot have an unknown reason");
    return QL_STATUS_INVALID_ARGUMENT;
  }
  recorded = answer_from_solver(answer);
  switch (target) {
  case QL_LOOP_PROOF_CHECK_INDUCTION:
    if (query->induction == NULL) {
      ql_error_set(error, QL_STATUS_NOT_FOUND,
                   "loop-proof query has no induction terminal");
      return QL_STATUS_NOT_FOUND;
    }
    query->view.metrics.induction_answer = recorded;
    query->view.metrics.stage_reached |= QL_LOOP_STAGE_INDUCTION;
    query->view.metrics.induction_solver_ns += elapsed;
    if (answer == QL_SOLVER_CHECK_UNSAT) {
      query->view.strategy = QL_LOOP_PROOF_STRATEGY_STRUCTURAL_INDUCTION;
      if (query->view.requires_concrete_domain_check == 0u) {
        copy_diagnostic(query->view.diagnostic,
                        "combined Base, guard-alignment, Step, and Exit "
                        "obligations are unsatisfiable");
      }
    } else if (answer == QL_SOLVER_CHECK_SAT && query->summary != NULL) {
      query->view.promotion_eligible = 0u;
      query->view.strategy = QL_LOOP_PROOF_STRATEGY_AFFINE_SUMMARY;
      copy_diagnostic(query->view.diagnostic,
                      "the structural invariant candidate was rejected; the "
                      "fixed-additive summary is next");
    } else {
      query->view.promotion_eligible = 0u;
      mark_fallback(
          query, QL_LOOP_PROOF_UNSUPPORTED_AFFINE_SUMMARY,
          answer == QL_SOLVER_CHECK_SAT
              ? "SAT rejected the invariant candidate; no affine summary or "
                "CHC/PDR backend is available"
              : "structural induction was not decided and no CHC/PDR backend "
                "is available");
    }
    break;
  case QL_LOOP_PROOF_CHECK_SUMMARY:
    if (query->summary == NULL) {
      ql_error_set(error, QL_STATUS_NOT_FOUND,
                   "loop-proof query has no summary terminal");
      return QL_STATUS_NOT_FOUND;
    }
    if (query->view.disposition != QL_LOOP_PROOF_QUERY_READY ||
        query->view.strategy != QL_LOOP_PROOF_STRATEGY_AFFINE_SUMMARY ||
        query->view.metrics.induction_answer != QL_SMT_PRODUCT_ANSWER_SAT) {
      ql_error_set(error, QL_STATUS_INVALID_ARGUMENT,
                   "summary can be checked only after SAT rejects induction");
      return QL_STATUS_INVALID_ARGUMENT;
    }
    query->view.metrics.summary_answer = recorded;
    query->view.metrics.stage_reached |= QL_LOOP_STAGE_SUMMARY;
    query->view.metrics.summary_solver_ns += elapsed;
    ++query->view.metrics.summary_attempted_count;
    if (answer == QL_SOLVER_CHECK_UNSAT) {
      query->view.strategy = QL_LOOP_PROOF_STRATEGY_AFFINE_SUMMARY;
      query->view.promotion_eligible = query->promotion_gate_satisfied;
      copy_diagnostic(query->view.diagnostic,
                      "the fixed-additive recurrence summary is "
                      "unsatisfiable");
    } else {
      query->view.promotion_eligible = 0u;
      mark_fallback(
          query, QL_LOOP_PROOF_UNSUPPORTED_AFFINE_SUMMARY,
          answer == QL_SOLVER_CHECK_SAT
              ? "SAT rejected the affine summary candidate; CHC/PDR was "
                "reached but is unavailable"
              : "the affine summary was not decided; CHC/PDR was reached but "
                "is unavailable");
    }
    break;
  case QL_LOOP_PROOF_CHECK_DOMAIN:
    if (query->domain == NULL ||
        query->view.requires_concrete_domain_check != 0u) {
      ql_error_set(error, QL_STATUS_NOT_FOUND,
                   "loop-proof query has no concrete domain terminal");
      return QL_STATUS_NOT_FOUND;
    }
    query->view.metrics.domain_answer = recorded;
    query->view.metrics.domain_solver_ns += elapsed;
    break;
  case QL_LOOP_PROOF_CHECK_REFLEXIVITY:
    if (query->reflexivity == NULL ||
        query->view.strategy != QL_LOOP_PROOF_STRATEGY_EXACT_REFLEXIVITY) {
      ql_error_set(error, QL_STATUS_NOT_FOUND,
                   "loop-proof query has no exact-reflexivity terminal");
      return QL_STATUS_NOT_FOUND;
    }
    query->view.metrics.reflexivity_answer = recorded;
    query->view.metrics.stage_reached |= QL_LOOP_STAGE_REFLEXIVITY;
    query->view.metrics.reflexivity_solver_ns += elapsed;
    if (answer == QL_SOLVER_CHECK_UNSAT) {
      copy_diagnostic(
          query->view.diagnostic,
          "the exact whole-IR reflexivity obligation is unsatisfiable and "
          "the concrete defined witness inhabits the comparison domain");
    } else {
      query->view.promotion_eligible = 0u;
      mark_fallback(
          query, QL_LOOP_PROOF_UNSUPPORTED_ENTRY_TRANSITION_EXIT_MISMATCH,
          "the exact whole-IR reflexivity obligation was not discharged; "
          "CHC/PDR is unavailable");
    }
    break;
  default:
    ql_error_set(error, QL_STATUS_INVALID_ARGUMENT,
                 "unknown loop-proof check target");
    return QL_STATUS_INVALID_ARGUMENT;
  }
  ql_error_clear(error);
  return QL_STATUS_OK;
}
