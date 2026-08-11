#include "quodlibet/ir.h"

#include "internal.h"

#include <stdatomic.h>
#include <string.h>

typedef struct ir_type_entry {
  ql_ir_type_kind kind;
  ql_ir_float_format float_format;
  uint32_t bit_width;
  uint32_t address_space;
  ql_ir_type_id element_type;
  uint64_t element_count;
} ir_type_entry;

typedef struct ir_value_entry {
  ql_ir_type_id type;
  ql_ir_value_definition_kind definition_kind;
  uint32_t result_index;
  ql_ir_instruction_id instruction;
  uint8_t *constant_data;
  size_t constant_size;
  char *name;
  size_t name_size;
} ir_value_entry;

typedef struct ir_instruction_entry {
  ql_ir_block_id block;
  ql_ir_opcode opcode;
  uint32_t flags;
  uint64_t effects;
  ql_ir_value_id *operands;
  size_t operand_count;
  ql_ir_block_id *block_operands;
  size_t block_operand_count;
  ql_ir_value_id *results;
  size_t result_count;
  uint64_t immediate;
  char *symbol;
  size_t symbol_size;
  void *image;
  size_t image_size;
} ir_instruction_entry;

typedef struct ir_terminator_entry {
  uint32_t present;
  ql_ir_terminator_kind kind;
  ql_ir_value_id condition;
  ql_ir_value_id return_value;
  ql_ir_value_id memory;
  ql_ir_value_id event_trace;
  ql_ir_block_id target;
  ql_ir_block_id false_target;
  uint64_t code;
  char *reason;
  size_t reason_size;
} ir_terminator_entry;

typedef struct ir_block_entry {
  char *label;
  size_t label_size;
  ql_ir_instruction_id *instructions;
  size_t instruction_count;
  size_t instruction_capacity;
  ir_terminator_entry terminator;
} ir_block_entry;

typedef struct ir_graph {
  ql_allocator allocator;
  ql_ir_cfg_kind cfg_kind;
  char *function_name;
  size_t function_name_size;
  ql_ir_type_id return_type;
  ql_ir_block_id entry_block;
  uint32_t function_is_set;
  uint32_t parameters_are_sealed;
  ir_type_entry *types;
  size_t type_count;
  size_t type_capacity;
  ir_value_entry *values;
  size_t value_count;
  size_t value_capacity;
  ir_instruction_entry *instructions;
  size_t instruction_count;
  size_t instruction_capacity;
  ir_block_entry *blocks;
  size_t block_count;
  size_t block_capacity;
} ir_graph;

struct ql_ir_builder {
  ir_graph graph;
};

struct ql_ir {
  atomic_uint reference_count;
  ir_graph graph;
  ql_artifact *artifact;
  ql_digest artifact_digest;
};

static const uint8_t ir_magic[8] = {UINT8_C(0x51), UINT8_C(0x4c), UINT8_C(0x49),
                                    UINT8_C(0x52), UINT8_C(0x0d), UINT8_C(0x0a),
                                    UINT8_C(0x1a), UINT8_C(0x0a)};

static int checked_add_size(size_t left, size_t right, size_t *result) {
  if (right > SIZE_MAX - left) {
    return 0;
  }
  *result = left + right;
  return 1;
}

static int checked_mul_size(size_t left, size_t right, size_t *result) {
  if (left != 0u && right > SIZE_MAX / left) {
    return 0;
  }
  *result = left * right;
  return 1;
}

static ql_status reserve_array(const ql_allocator *allocator, void **items,
                               size_t *capacity, size_t needed,
                               size_t item_size, ql_error *error) {
  size_t new_capacity;
  size_t byte_size;
  void *new_items;

  if (needed <= *capacity) {
    return QL_STATUS_OK;
  }
  new_capacity = *capacity == 0u ? 8u : *capacity;
  while (new_capacity < needed) {
    if (new_capacity > SIZE_MAX / 2u) {
      new_capacity = needed;
      break;
    }
    new_capacity *= 2u;
  }
  if (!checked_mul_size(new_capacity, item_size, &byte_size)) {
    ql_error_set(error, QL_STATUS_INVALID_ARGUMENT,
                 "IR collection size overflows size_t");
    return QL_STATUS_INVALID_ARGUMENT;
  }
  new_items = allocator->reallocate(allocator->user_data, *items, byte_size);
  if (new_items == NULL) {
    ql_error_set(error, QL_STATUS_OUT_OF_MEMORY, NULL);
    return QL_STATUS_OUT_OF_MEMORY;
  }
  *items = new_items;
  *capacity = new_capacity;
  return QL_STATUS_OK;
}

static ql_status copy_bytes(const ql_allocator *allocator, const void *data,
                            size_t size, void **output, ql_error *error) {
  void *copy = NULL;

  *output = NULL;
  if (size == 0u) {
    return QL_STATUS_OK;
  }
  if (data == NULL) {
    ql_error_set(error, QL_STATUS_INVALID_ARGUMENT,
                 "non-empty IR bytes require a source pointer");
    return QL_STATUS_INVALID_ARGUMENT;
  }
  copy = allocator->allocate(allocator->user_data, size);
  if (copy == NULL) {
    ql_error_set(error, QL_STATUS_OUT_OF_MEMORY, NULL);
    return QL_STATUS_OUT_OF_MEMORY;
  }
  memcpy(copy, data, size);
  *output = copy;
  return QL_STATUS_OK;
}

static ql_status copy_text(const ql_allocator *allocator, const char *text,
                           size_t size, char **output, ql_error *error) {
  size_t allocation_size;
  char *copy;

  *output = NULL;
  if (size == 0u) {
    return QL_STATUS_OK;
  }
  if (text == NULL) {
    ql_error_set(error, QL_STATUS_INVALID_ARGUMENT,
                 "non-empty IR text requires a source pointer");
    return QL_STATUS_INVALID_ARGUMENT;
  }
  if (!checked_add_size(size, 1u, &allocation_size)) {
    ql_error_set(error, QL_STATUS_INVALID_ARGUMENT,
                 "IR text size overflows size_t");
    return QL_STATUS_INVALID_ARGUMENT;
  }
  if (memchr(text, '\0', size) != NULL) {
    ql_error_set(error, QL_STATUS_INVALID_ARGUMENT,
                 "IR text contains an embedded NUL");
    return QL_STATUS_INVALID_ARGUMENT;
  }
  copy = allocator->allocate(allocator->user_data, allocation_size);
  if (copy == NULL) {
    ql_error_set(error, QL_STATUS_OUT_OF_MEMORY, NULL);
    return QL_STATUS_OUT_OF_MEMORY;
  }
  memcpy(copy, text, size);
  copy[size] = '\0';
  *output = copy;
  return QL_STATUS_OK;
}

static void graph_init(ir_graph *graph, const ql_allocator *allocator) {
  memset(graph, 0, sizeof(*graph));
  graph->allocator = *allocator;
  graph->cfg_kind = QL_IR_CFG_ACYCLIC;
  graph->return_type = QL_IR_INVALID_TYPE_ID;
  graph->entry_block = QL_IR_INVALID_BLOCK_ID;
}

static void free_instruction(const ql_allocator *allocator,
                             ir_instruction_entry *instruction) {
  allocator->deallocate(allocator->user_data, instruction->image);
  allocator->deallocate(allocator->user_data, instruction->symbol);
  allocator->deallocate(allocator->user_data, instruction->results);
  allocator->deallocate(allocator->user_data, instruction->block_operands);
  allocator->deallocate(allocator->user_data, instruction->operands);
  memset(instruction, 0, sizeof(*instruction));
}

static void graph_dispose(ir_graph *graph) {
  size_t index;
  ql_allocator allocator = graph->allocator;

  for (index = 0u; index < graph->value_count; ++index) {
    allocator.deallocate(allocator.user_data,
                         graph->values[index].constant_data);
    allocator.deallocate(allocator.user_data, graph->values[index].name);
  }
  for (index = 0u; index < graph->instruction_count; ++index) {
    free_instruction(&allocator, &graph->instructions[index]);
  }
  for (index = 0u; index < graph->block_count; ++index) {
    allocator.deallocate(allocator.user_data,
                         graph->blocks[index].terminator.reason);
    allocator.deallocate(allocator.user_data,
                         graph->blocks[index].instructions);
    allocator.deallocate(allocator.user_data, graph->blocks[index].label);
  }
  allocator.deallocate(allocator.user_data, graph->blocks);
  allocator.deallocate(allocator.user_data, graph->instructions);
  allocator.deallocate(allocator.user_data, graph->values);
  allocator.deallocate(allocator.user_data, graph->types);
  allocator.deallocate(allocator.user_data, graph->function_name);
  memset(graph, 0, sizeof(*graph));
}

static uint32_t float_format_width(ql_ir_float_format format) {
  switch (format) {
  case QL_IR_FLOAT_IEEE_BINARY16:
  case QL_IR_FLOAT_BFLOAT16:
    return 16u;
  case QL_IR_FLOAT_IEEE_BINARY32:
    return 32u;
  case QL_IR_FLOAT_IEEE_BINARY64:
    return 64u;
  case QL_IR_FLOAT_X87_BINARY80:
    return 80u;
  case QL_IR_FLOAT_IEEE_BINARY128:
    return 128u;
  default:
    return 0u;
  }
}

static ql_status
validate_type_definition(const ir_graph *graph,
                         const ql_ir_type_definition_v1 *definition,
                         ql_error *error) {
  if (definition == NULL || definition->struct_size < sizeof(*definition)) {
    ql_error_set(error, QL_STATUS_ABI_MISMATCH,
                 "IR type definition v1 has an invalid size");
    return QL_STATUS_ABI_MISMATCH;
  }
  switch (definition->kind) {
  case QL_IR_TYPE_VOID:
  case QL_IR_TYPE_MEMORY:
  case QL_IR_TYPE_EVENT_TRACE:
    if (definition->float_format != QL_IR_FLOAT_INVALID ||
        definition->bit_width != 0u || definition->address_space != 0u ||
        definition->element_type != QL_IR_INVALID_TYPE_ID ||
        definition->element_count != 0u) {
      break;
    }
    return QL_STATUS_OK;
  case QL_IR_TYPE_BOOL:
    if (definition->float_format == QL_IR_FLOAT_INVALID &&
        definition->bit_width == 1u && definition->address_space == 0u &&
        definition->element_type == QL_IR_INVALID_TYPE_ID &&
        definition->element_count == 0u) {
      return QL_STATUS_OK;
    }
    break;
  case QL_IR_TYPE_BIT_VECTOR:
    if (definition->float_format == QL_IR_FLOAT_INVALID &&
        definition->bit_width != 0u && definition->address_space == 0u &&
        definition->element_type == QL_IR_INVALID_TYPE_ID &&
        definition->element_count == 0u) {
      return QL_STATUS_OK;
    }
    break;
  case QL_IR_TYPE_FLOAT:
    if (float_format_width(definition->float_format) == definition->bit_width &&
        definition->bit_width != 0u && definition->address_space == 0u &&
        definition->element_type == QL_IR_INVALID_TYPE_ID &&
        definition->element_count == 0u) {
      return QL_STATUS_OK;
    }
    break;
  case QL_IR_TYPE_POINTER:
    if (definition->float_format == QL_IR_FLOAT_INVALID &&
        definition->bit_width != 0u &&
        definition->element_type < graph->type_count &&
        definition->element_count == 0u &&
        graph->types[definition->element_type].kind != QL_IR_TYPE_MEMORY &&
        graph->types[definition->element_type].kind != QL_IR_TYPE_EVENT_TRACE) {
      return QL_STATUS_OK;
    }
    break;
  case QL_IR_TYPE_ARRAY:
  case QL_IR_TYPE_TUPLE:
  default:
    ql_error_set(error, QL_STATUS_INVALID_ARGUMENT,
                 "IR schema v1 does not support type kind %u",
                 (unsigned)definition->kind);
    return QL_STATUS_INVALID_ARGUMENT;
  }
  ql_error_set(error, QL_STATUS_INVALID_ARGUMENT,
               "invalid fields for IR type kind %u",
               (unsigned)definition->kind);
  return QL_STATUS_INVALID_ARGUMENT;
}

static ql_status validate_stored_type(const ir_graph *graph, size_t index,
                                      ql_error *error) {
  ql_ir_type_definition_v1 definition;
  const ir_type_entry *type = &graph->types[index];

  memset(&definition, 0, sizeof(definition));
  definition.struct_size = sizeof(definition);
  definition.kind = type->kind;
  definition.float_format = type->float_format;
  definition.bit_width = type->bit_width;
  definition.address_space = type->address_space;
  definition.element_type = type->element_type;
  definition.element_count = type->element_count;
  return validate_type_definition(graph, &definition, error);
}

void QL_CALL ql_ir_type_definition_init(ql_ir_type_definition_v1 *definition,
                                        ql_ir_type_kind kind) {
  if (definition == NULL) {
    return;
  }
  memset(definition, 0, sizeof(*definition));
  definition->struct_size = sizeof(*definition);
  definition->kind = kind;
  definition->float_format = QL_IR_FLOAT_INVALID;
  definition->element_type = QL_IR_INVALID_TYPE_ID;
  if (kind == QL_IR_TYPE_BOOL) {
    definition->bit_width = 1u;
  }
}

void QL_CALL ql_ir_instruction_definition_init(
    ql_ir_instruction_definition_v1 *definition, ql_ir_opcode opcode) {
  if (definition == NULL) {
    return;
  }
  memset(definition, 0, sizeof(*definition));
  definition->struct_size = sizeof(*definition);
  definition->opcode = opcode;
}

void QL_CALL ql_ir_terminator_definition_init(
    ql_ir_terminator_definition_v1 *definition, ql_ir_terminator_kind kind) {
  if (definition == NULL) {
    return;
  }
  memset(definition, 0, sizeof(*definition));
  definition->struct_size = sizeof(*definition);
  definition->kind = kind;
  definition->condition = QL_IR_INVALID_VALUE_ID;
  definition->return_value = QL_IR_INVALID_VALUE_ID;
  definition->memory = QL_IR_INVALID_VALUE_ID;
  definition->event_trace = QL_IR_INVALID_VALUE_ID;
  definition->target = QL_IR_INVALID_BLOCK_ID;
  definition->false_target = QL_IR_INVALID_BLOCK_ID;
}

ql_status QL_CALL ql_ir_builder_create(const ql_allocator *allocator,
                                       ql_ir_builder **output,
                                       ql_error *error) {
  const ql_allocator *selected = allocator;
  ql_ir_builder *builder;

  if (output == NULL) {
    ql_error_set(error, QL_STATUS_INVALID_ARGUMENT,
                 "IR builder output is required");
    return QL_STATUS_INVALID_ARGUMENT;
  }
  *output = NULL;
  if (selected == NULL) {
    selected = ql_default_allocator();
  }
  if (!ql_allocator_is_valid(selected)) {
    ql_error_set(error, QL_STATUS_INVALID_ARGUMENT, "invalid allocator");
    return QL_STATUS_INVALID_ARGUMENT;
  }
  builder = selected->allocate(selected->user_data, sizeof(*builder));
  if (builder == NULL) {
    ql_error_set(error, QL_STATUS_OUT_OF_MEMORY, NULL);
    return QL_STATUS_OUT_OF_MEMORY;
  }
  graph_init(&builder->graph, selected);
  *output = builder;
  ql_error_clear(error);
  return QL_STATUS_OK;
}

void QL_CALL ql_ir_builder_destroy(ql_ir_builder *builder) {
  ql_allocator allocator;

  if (builder == NULL) {
    return;
  }
  allocator = builder->graph.allocator;
  graph_dispose(&builder->graph);
  allocator.deallocate(allocator.user_data, builder);
}

ql_status QL_CALL ql_ir_builder_add_type(
    ql_ir_builder *builder, const ql_ir_type_definition_v1 *definition,
    ql_ir_type_id *output, ql_error *error) {
  ir_type_entry *entry;
  ql_status status;

  if (builder == NULL || output == NULL) {
    ql_error_set(error, QL_STATUS_INVALID_ARGUMENT,
                 "IR builder and type output are required");
    return QL_STATUS_INVALID_ARGUMENT;
  }
  *output = QL_IR_INVALID_TYPE_ID;
  if (builder->graph.type_count >= (size_t)UINT32_MAX) {
    ql_error_set(error, QL_STATUS_INVALID_ARGUMENT,
                 "IR type ID space is exhausted");
    return QL_STATUS_INVALID_ARGUMENT;
  }
  status = validate_type_definition(&builder->graph, definition, error);
  if (status != QL_STATUS_OK) {
    return status;
  }
  status = reserve_array(
      &builder->graph.allocator, (void **)&builder->graph.types,
      &builder->graph.type_capacity, builder->graph.type_count + 1u,
      sizeof(*builder->graph.types), error);
  if (status != QL_STATUS_OK) {
    return status;
  }
  entry = &builder->graph.types[builder->graph.type_count];
  entry->kind = definition->kind;
  entry->float_format = definition->float_format;
  entry->bit_width = definition->bit_width;
  entry->address_space = definition->address_space;
  entry->element_type = definition->element_type;
  entry->element_count = definition->element_count;
  *output = (ql_ir_type_id)builder->graph.type_count;
  ++builder->graph.type_count;
  ql_error_clear(error);
  return QL_STATUS_OK;
}

ql_status QL_CALL ql_ir_builder_set_function(ql_ir_builder *builder,
                                             const char *name, size_t name_size,
                                             ql_ir_type_id return_type,
                                             ql_error *error) {
  char *name_copy;
  ql_status status;

  if (builder == NULL || name == NULL || name_size == 0u) {
    ql_error_set(error, QL_STATUS_INVALID_ARGUMENT,
                 "IR function requires a non-empty name");
    return QL_STATUS_INVALID_ARGUMENT;
  }
  if (builder->graph.function_is_set != 0u) {
    ql_error_set(error, QL_STATUS_ALREADY_EXISTS,
                 "IR function metadata is already set");
    return QL_STATUS_ALREADY_EXISTS;
  }
  if (return_type >= builder->graph.type_count ||
      builder->graph.types[return_type].kind == QL_IR_TYPE_MEMORY ||
      builder->graph.types[return_type].kind == QL_IR_TYPE_EVENT_TRACE) {
    ql_error_set(error, QL_STATUS_TYPE_MISMATCH,
                 "invalid IR function return type %u", return_type);
    return QL_STATUS_TYPE_MISMATCH;
  }
  status =
      copy_text(&builder->graph.allocator, name, name_size, &name_copy, error);
  if (status != QL_STATUS_OK) {
    return status;
  }
  builder->graph.function_name = name_copy;
  builder->graph.function_name_size = name_size;
  builder->graph.return_type = return_type;
  builder->graph.function_is_set = 1u;
  ql_error_clear(error);
  return QL_STATUS_OK;
}

ql_status QL_CALL ql_ir_builder_set_cfg_kind(ql_ir_builder *builder,
                                             ql_ir_cfg_kind kind,
                                             ql_error *error) {
  if (builder == NULL) {
    ql_error_set(error, QL_STATUS_INVALID_ARGUMENT, "IR builder is required");
    return QL_STATUS_INVALID_ARGUMENT;
  }
  if (kind != QL_IR_CFG_ACYCLIC && kind != QL_IR_CFG_CYCLIC) {
    ql_error_set(error, QL_STATUS_INVALID_ARGUMENT, "unknown IR CFG kind %u",
                 (unsigned)kind);
    return QL_STATUS_INVALID_ARGUMENT;
  }
  builder->graph.cfg_kind = kind;
  ql_error_clear(error);
  return QL_STATUS_OK;
}

static ql_status validate_value_type(const ir_graph *graph, ql_ir_type_id type,
                                     int allow_state_tokens, ql_error *error) {
  ql_ir_type_kind kind;

  if (type >= graph->type_count) {
    ql_error_set(error, QL_STATUS_INVALID_ARGUMENT,
                 "IR references missing type %u", type);
    return QL_STATUS_INVALID_ARGUMENT;
  }
  kind = graph->types[type].kind;
  if (kind == QL_IR_TYPE_VOID ||
      (!allow_state_tokens &&
       (kind == QL_IR_TYPE_MEMORY || kind == QL_IR_TYPE_EVENT_TRACE))) {
    ql_error_set(error, QL_STATUS_TYPE_MISMATCH,
                 "IR type %u cannot define this value", type);
    return QL_STATUS_TYPE_MISMATCH;
  }
  return QL_STATUS_OK;
}

ql_status QL_CALL ql_ir_builder_add_parameter(
    ql_ir_builder *builder, ql_ir_type_id type, const char *name,
    size_t name_size, ql_ir_value_id *output, ql_error *error) {
  ir_value_entry *entry;
  char *name_copy;
  ql_status status;

  if (builder == NULL || output == NULL) {
    ql_error_set(error, QL_STATUS_INVALID_ARGUMENT,
                 "IR builder and parameter output are required");
    return QL_STATUS_INVALID_ARGUMENT;
  }
  *output = QL_IR_INVALID_VALUE_ID;
  if (builder->graph.parameters_are_sealed != 0u) {
    ql_error_set(error, QL_STATUS_INVALID_ARGUMENT,
                 "IR parameters must precede constants and instructions");
    return QL_STATUS_INVALID_ARGUMENT;
  }
  if (builder->graph.value_count >= (size_t)UINT32_MAX) {
    ql_error_set(error, QL_STATUS_INVALID_ARGUMENT,
                 "IR value ID space is exhausted");
    return QL_STATUS_INVALID_ARGUMENT;
  }
  status = validate_value_type(&builder->graph, type, 1, error);
  if (status != QL_STATUS_OK) {
    return status;
  }
  status =
      copy_text(&builder->graph.allocator, name, name_size, &name_copy, error);
  if (status != QL_STATUS_OK) {
    return status;
  }
  status = reserve_array(
      &builder->graph.allocator, (void **)&builder->graph.values,
      &builder->graph.value_capacity, builder->graph.value_count + 1u,
      sizeof(*builder->graph.values), error);
  if (status != QL_STATUS_OK) {
    builder->graph.allocator.deallocate(builder->graph.allocator.user_data,
                                        name_copy);
    return status;
  }
  entry = &builder->graph.values[builder->graph.value_count];
  memset(entry, 0, sizeof(*entry));
  entry->type = type;
  entry->definition_kind = QL_IR_VALUE_PARAMETER;
  entry->instruction = QL_IR_INVALID_INSTRUCTION_ID;
  entry->name = name_copy;
  entry->name_size = name_size;
  *output = (ql_ir_value_id)builder->graph.value_count;
  ++builder->graph.value_count;
  ql_error_clear(error);
  return QL_STATUS_OK;
}

ql_status ql_internal_ir_builder_add_late_parameter(
    ql_ir_builder *builder, ql_ir_type_id type, const char *name,
    size_t name_size, ql_ir_value_id *output, ql_error *error) {
  uint32_t sealed;
  ql_status status;

  if (builder == NULL) {
    ql_error_set(error, QL_STATUS_INVALID_ARGUMENT, "IR builder is required");
    return QL_STATUS_INVALID_ARGUMENT;
  }
  sealed = builder->graph.parameters_are_sealed;
  builder->graph.parameters_are_sealed = 0u;
  status = ql_ir_builder_add_parameter(builder, type, name, name_size, output,
                                       error);
  builder->graph.parameters_are_sealed = sealed;
  return status;
}

ql_status ql_internal_ir_builder_replace_entry_block(ql_ir_builder *builder,
                                                     ql_ir_block_id expected,
                                                     ql_ir_block_id replacement,
                                                     ql_error *error) {
  if (builder == NULL || builder->graph.entry_block != expected ||
      replacement >= builder->graph.block_count) {
    ql_error_set(error, QL_STATUS_INVALID_ARGUMENT,
                 "IR entry replacement does not match the builder");
    return QL_STATUS_INVALID_ARGUMENT;
  }
  builder->graph.entry_block = replacement;
  ql_error_clear(error);
  return QL_STATUS_OK;
}

static ql_status constant_expected_size(const ir_graph *graph,
                                        ql_ir_type_id type, size_t *size,
                                        ql_error *error) {
  const ir_type_entry *entry;
  uint32_t width;

  if (type >= graph->type_count) {
    ql_error_set(error, QL_STATUS_INVALID_ARGUMENT,
                 "IR constant references missing type %u", type);
    return QL_STATUS_INVALID_ARGUMENT;
  }
  entry = &graph->types[type];
  if (entry->kind != QL_IR_TYPE_BOOL && entry->kind != QL_IR_TYPE_BIT_VECTOR &&
      entry->kind != QL_IR_TYPE_FLOAT && entry->kind != QL_IR_TYPE_POINTER) {
    ql_error_set(error, QL_STATUS_TYPE_MISMATCH,
                 "IR type %u cannot have a constant", type);
    return QL_STATUS_TYPE_MISMATCH;
  }
  width = entry->kind == QL_IR_TYPE_BOOL ? 1u : entry->bit_width;
  *size = (size_t)(width / 8u) + (width % 8u == 0u ? 0u : 1u);
  return QL_STATUS_OK;
}

static ql_status validate_constant(const ir_graph *graph, ql_ir_type_id type,
                                   const void *data, size_t size,
                                   ql_error *error) {
  const uint8_t *bytes = (const uint8_t *)data;
  size_t expected_size;
  uint32_t width;
  uint32_t unused_bits;
  ql_status status = constant_expected_size(graph, type, &expected_size, error);

  if (status != QL_STATUS_OK) {
    return status;
  }
  if (data == NULL || size != expected_size) {
    ql_error_set(error, QL_STATUS_INVALID_ARGUMENT,
                 "IR constant type %u requires %zu bytes", type, expected_size);
    return QL_STATUS_INVALID_ARGUMENT;
  }
  if (graph->types[type].kind == QL_IR_TYPE_BOOL && bytes[0] > 1u) {
    ql_error_set(error, QL_STATUS_INVALID_ARGUMENT,
                 "IR boolean constant must be zero or one");
    return QL_STATUS_INVALID_ARGUMENT;
  }
  width = graph->types[type].kind == QL_IR_TYPE_BOOL
              ? 1u
              : graph->types[type].bit_width;
  unused_bits = (uint32_t)(expected_size * 8u) - width;
  if (unused_bits != 0u &&
      (bytes[expected_size - 1u] >> (8u - unused_bits)) != 0u) {
    ql_error_set(error, QL_STATUS_INVALID_ARGUMENT,
                 "IR constant has non-zero unused high bits");
    return QL_STATUS_INVALID_ARGUMENT;
  }
  if (graph->types[type].kind == QL_IR_TYPE_POINTER) {
    size_t index;
    for (index = 0u; index < size; ++index) {
      if (bytes[index] != 0u) {
        ql_error_set(error, QL_STATUS_INVALID_ARGUMENT,
                     "IR pointer constants are limited to null in v1");
        return QL_STATUS_INVALID_ARGUMENT;
      }
    }
  }
  return QL_STATUS_OK;
}

ql_status QL_CALL ql_ir_builder_add_constant(ql_ir_builder *builder,
                                             ql_ir_type_id type,
                                             const void *data, size_t size,
                                             ql_ir_value_id *output,
                                             ql_error *error) {
  ir_value_entry *entry;
  void *data_copy;
  ql_status status;

  if (builder == NULL || output == NULL) {
    ql_error_set(error, QL_STATUS_INVALID_ARGUMENT,
                 "IR builder and constant output are required");
    return QL_STATUS_INVALID_ARGUMENT;
  }
  *output = QL_IR_INVALID_VALUE_ID;
  if (builder->graph.value_count >= (size_t)UINT32_MAX) {
    ql_error_set(error, QL_STATUS_INVALID_ARGUMENT,
                 "IR value ID space is exhausted");
    return QL_STATUS_INVALID_ARGUMENT;
  }
  status = validate_constant(&builder->graph, type, data, size, error);
  if (status != QL_STATUS_OK) {
    return status;
  }
  status = copy_bytes(&builder->graph.allocator, data, size, &data_copy, error);
  if (status != QL_STATUS_OK) {
    return status;
  }
  status = reserve_array(
      &builder->graph.allocator, (void **)&builder->graph.values,
      &builder->graph.value_capacity, builder->graph.value_count + 1u,
      sizeof(*builder->graph.values), error);
  if (status != QL_STATUS_OK) {
    builder->graph.allocator.deallocate(builder->graph.allocator.user_data,
                                        data_copy);
    return status;
  }
  entry = &builder->graph.values[builder->graph.value_count];
  memset(entry, 0, sizeof(*entry));
  entry->type = type;
  entry->definition_kind = QL_IR_VALUE_CONSTANT;
  entry->instruction = QL_IR_INVALID_INSTRUCTION_ID;
  entry->constant_data = (uint8_t *)data_copy;
  entry->constant_size = size;
  *output = (ql_ir_value_id)builder->graph.value_count;
  ++builder->graph.value_count;
  builder->graph.parameters_are_sealed = 1u;
  ql_error_clear(error);
  return QL_STATUS_OK;
}

ql_status QL_CALL ql_ir_builder_add_block(ql_ir_builder *builder,
                                          const char *label, size_t label_size,
                                          ql_ir_block_id *output,
                                          ql_error *error) {
  ir_block_entry *entry;
  char *label_copy;
  ql_status status;

  if (builder == NULL || output == NULL) {
    ql_error_set(error, QL_STATUS_INVALID_ARGUMENT,
                 "IR builder and block output are required");
    return QL_STATUS_INVALID_ARGUMENT;
  }
  *output = QL_IR_INVALID_BLOCK_ID;
  if (builder->graph.block_count >= (size_t)UINT32_MAX) {
    ql_error_set(error, QL_STATUS_INVALID_ARGUMENT,
                 "IR block ID space is exhausted");
    return QL_STATUS_INVALID_ARGUMENT;
  }
  status = copy_text(&builder->graph.allocator, label, label_size, &label_copy,
                     error);
  if (status != QL_STATUS_OK) {
    return status;
  }
  status = reserve_array(
      &builder->graph.allocator, (void **)&builder->graph.blocks,
      &builder->graph.block_capacity, builder->graph.block_count + 1u,
      sizeof(*builder->graph.blocks), error);
  if (status != QL_STATUS_OK) {
    builder->graph.allocator.deallocate(builder->graph.allocator.user_data,
                                        label_copy);
    return status;
  }
  entry = &builder->graph.blocks[builder->graph.block_count];
  memset(entry, 0, sizeof(*entry));
  entry->label = label_copy;
  entry->label_size = label_size;
  entry->terminator.condition = QL_IR_INVALID_VALUE_ID;
  entry->terminator.return_value = QL_IR_INVALID_VALUE_ID;
  entry->terminator.memory = QL_IR_INVALID_VALUE_ID;
  entry->terminator.event_trace = QL_IR_INVALID_VALUE_ID;
  entry->terminator.target = QL_IR_INVALID_BLOCK_ID;
  entry->terminator.false_target = QL_IR_INVALID_BLOCK_ID;
  *output = (ql_ir_block_id)builder->graph.block_count;
  ++builder->graph.block_count;
  builder->graph.parameters_are_sealed = 1u;
  ql_error_clear(error);
  return QL_STATUS_OK;
}

ql_status QL_CALL ql_ir_builder_set_entry_block(ql_ir_builder *builder,
                                                ql_ir_block_id block,
                                                ql_error *error) {
  if (builder == NULL || block >= builder->graph.block_count) {
    ql_error_set(error, QL_STATUS_INVALID_ARGUMENT,
                 "IR entry block reference is invalid");
    return QL_STATUS_INVALID_ARGUMENT;
  }
  if (builder->graph.entry_block != QL_IR_INVALID_BLOCK_ID) {
    ql_error_set(error, QL_STATUS_ALREADY_EXISTS,
                 "IR entry block is already set");
    return QL_STATUS_ALREADY_EXISTS;
  }
  builder->graph.entry_block = block;
  ql_error_clear(error);
  return QL_STATUS_OK;
}

static int valid_effects(uint64_t effects) {
  const uint64_t known = QL_IR_EFFECT_MEMORY | QL_IR_EFFECT_CALL |
                         QL_IR_EFFECT_VOLATILE | QL_IR_EFFECT_ATOMIC |
                         QL_IR_EFFECT_IO | QL_IR_EFFECT_UNDEFINED_BEHAVIOR;
  return (effects & ~known) == 0u;
}

static ql_ir_type_kind value_kind(const ir_graph *graph, ql_ir_value_id value) {
  return graph->types[graph->values[value].type].kind;
}

static ql_ir_type_id
instruction_result_type(const ir_graph *graph,
                        const ir_instruction_entry *instruction, size_t index) {
  return graph->values[instruction->results[index]].type;
}

static int all_operands_same_type(const ir_graph *graph,
                                  const ir_instruction_entry *instruction,
                                  ql_ir_type_id type) {
  size_t index;
  for (index = 0u; index < instruction->operand_count; ++index) {
    if (graph->values[instruction->operands[index]].type != type) {
      return 0;
    }
  }
  return 1;
}

static ql_status instruction_type_error(ql_ir_instruction_id id,
                                        const char *rule, ql_error *error) {
  ql_error_set(error, QL_STATUS_TYPE_MISMATCH,
               "IR instruction %u violates %s type rule", id, rule);
  return QL_STATUS_TYPE_MISMATCH;
}

static ql_status validate_instruction_shape(const ir_graph *graph,
                                            ql_ir_instruction_id id,
                                            ql_error *error) {
  const ir_instruction_entry *instruction = &graph->instructions[id];
  ql_ir_type_id first_type = QL_IR_INVALID_TYPE_ID;
  ql_ir_type_id result_type = QL_IR_INVALID_TYPE_ID;
  ql_ir_type_kind first_kind = QL_IR_TYPE_VOID;
  ql_ir_type_kind result_kind = QL_IR_TYPE_VOID;
  size_t index;

  if (!valid_effects(instruction->effects)) {
    ql_error_set(error, QL_STATUS_INVALID_ARGUMENT,
                 "IR instruction %u has unknown effect bits", id);
    return QL_STATUS_INVALID_ARGUMENT;
  }
  if (instruction->opcode == 0u ||
      (instruction->opcode > QL_IR_OPCODE_MEMORY_IMAGE &&
       instruction->opcode < QL_IR_OPCODE_EXTENSION_BASE)) {
    ql_error_set(error, QL_STATUS_INVALID_ARGUMENT,
                 "IR instruction %u has unknown opcode %u", id,
                 instruction->opcode);
    return QL_STATUS_INVALID_ARGUMENT;
  }
  if (instruction->opcode < QL_IR_OPCODE_EXTENSION_BASE &&
      instruction->flags != 0u) {
    ql_error_set(error, QL_STATUS_INVALID_ARGUMENT,
                 "built-in IR instruction %u has unsupported flags", id);
    return QL_STATUS_INVALID_ARGUMENT;
  }
  for (index = 0u; index < instruction->operand_count; ++index) {
    if (instruction->operands[index] >= graph->value_count) {
      ql_error_set(error, QL_STATUS_INVALID_ARGUMENT,
                   "IR instruction %u references missing value %u", id,
                   instruction->operands[index]);
      return QL_STATUS_INVALID_ARGUMENT;
    }
  }
  for (index = 0u; index < instruction->block_operand_count; ++index) {
    if (instruction->block_operands[index] >= graph->block_count) {
      ql_error_set(error, QL_STATUS_INVALID_ARGUMENT,
                   "IR instruction %u references missing block %u", id,
                   instruction->block_operands[index]);
      return QL_STATUS_INVALID_ARGUMENT;
    }
  }
  for (index = 0u; index < instruction->result_count; ++index) {
    ql_ir_value_id value = instruction->results[index];
    if (value >= graph->value_count ||
        graph->values[value].definition_kind !=
            QL_IR_VALUE_INSTRUCTION_RESULT ||
        graph->values[value].instruction != id ||
        graph->values[value].result_index != (uint32_t)index) {
      ql_error_set(error, QL_STATUS_INVALID_ARGUMENT,
                   "IR instruction %u has an invalid result reference", id);
      return QL_STATUS_INVALID_ARGUMENT;
    }
  }
  if (instruction->opcode != QL_IR_OPCODE_PHI &&
      instruction->block_operand_count != 0u) {
    ql_error_set(error, QL_STATUS_INVALID_ARGUMENT,
                 "only IR PHI instructions accept block operands");
    return QL_STATUS_INVALID_ARGUMENT;
  }
  if (instruction->operand_count != 0u) {
    first_type = graph->values[instruction->operands[0]].type;
    first_kind = graph->types[first_type].kind;
  }
  if (instruction->result_count != 0u) {
    result_type = instruction_result_type(graph, instruction, 0u);
    result_kind = graph->types[result_type].kind;
  }

#define REQUIRE_COUNTS(operands_, results_)                                    \
  do {                                                                         \
    if (instruction->operand_count != (operands_) ||                           \
        instruction->result_count != (results_)) {                             \
      return instruction_type_error(id, "operand/result arity", error);        \
    }                                                                          \
  } while (0)
#define REQUIRE_PURE()                                                         \
  do {                                                                         \
    if (instruction->effects != QL_IR_EFFECT_NONE) {                           \
      return instruction_type_error(id, "pure effect", error);                 \
    }                                                                          \
  } while (0)

  switch (instruction->opcode) {
  case QL_IR_OPCODE_IDENTITY:
    REQUIRE_COUNTS(1u, 1u);
    REQUIRE_PURE();
    if (first_type != result_type) {
      return instruction_type_error(id, "identity", error);
    }
    break;
  case QL_IR_OPCODE_PHI:
    if (instruction->operand_count == 0u || instruction->result_count != 1u ||
        instruction->block_operand_count != instruction->operand_count) {
      return instruction_type_error(id, "PHI arity", error);
    }
    REQUIRE_PURE();
    if (!all_operands_same_type(graph, instruction, result_type)) {
      return instruction_type_error(id, "PHI", error);
    }
    break;
  case QL_IR_OPCODE_BOOL_NOT:
    REQUIRE_COUNTS(1u, 1u);
    REQUIRE_PURE();
    if (first_kind != QL_IR_TYPE_BOOL || first_type != result_type) {
      return instruction_type_error(id, "boolean not", error);
    }
    break;
  case QL_IR_OPCODE_BV_NOT:
  case QL_IR_OPCODE_BV_NEG:
    REQUIRE_COUNTS(1u, 1u);
    REQUIRE_PURE();
    if (first_kind != QL_IR_TYPE_BIT_VECTOR || first_type != result_type) {
      return instruction_type_error(id, "bit-vector unary", error);
    }
    break;
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
    REQUIRE_COUNTS(2u, 1u);
    REQUIRE_PURE();
    if (first_kind != QL_IR_TYPE_BIT_VECTOR ||
        !all_operands_same_type(graph, instruction, first_type) ||
        result_type != first_type) {
      return instruction_type_error(id, "bit-vector binary", error);
    }
    break;
  case QL_IR_OPCODE_EQ:
  case QL_IR_OPCODE_NE:
    REQUIRE_COUNTS(2u, 1u);
    REQUIRE_PURE();
    if (!all_operands_same_type(graph, instruction, first_type) ||
        result_kind != QL_IR_TYPE_BOOL || first_kind == QL_IR_TYPE_VOID ||
        first_kind == QL_IR_TYPE_MEMORY ||
        first_kind == QL_IR_TYPE_EVENT_TRACE) {
      return instruction_type_error(id, "equality", error);
    }
    break;
  case QL_IR_OPCODE_ULT:
  case QL_IR_OPCODE_ULE:
  case QL_IR_OPCODE_SLT:
  case QL_IR_OPCODE_SLE:
    REQUIRE_COUNTS(2u, 1u);
    REQUIRE_PURE();
    if (first_kind != QL_IR_TYPE_BIT_VECTOR ||
        !all_operands_same_type(graph, instruction, first_type) ||
        result_kind != QL_IR_TYPE_BOOL) {
      return instruction_type_error(id, "integer comparison", error);
    }
    break;
  case QL_IR_OPCODE_SELECT:
    REQUIRE_COUNTS(3u, 1u);
    REQUIRE_PURE();
    if (first_kind != QL_IR_TYPE_BOOL ||
        graph->values[instruction->operands[1]].type != result_type ||
        graph->values[instruction->operands[2]].type != result_type) {
      return instruction_type_error(id, "select", error);
    }
    break;
  case QL_IR_OPCODE_ZEXT:
  case QL_IR_OPCODE_SEXT:
    REQUIRE_COUNTS(1u, 1u);
    REQUIRE_PURE();
    if (first_kind != QL_IR_TYPE_BIT_VECTOR ||
        result_kind != QL_IR_TYPE_BIT_VECTOR ||
        graph->types[result_type].bit_width <=
            graph->types[first_type].bit_width) {
      return instruction_type_error(id, "integer extension", error);
    }
    break;
  case QL_IR_OPCODE_TRUNC:
    REQUIRE_COUNTS(1u, 1u);
    REQUIRE_PURE();
    if (first_kind != QL_IR_TYPE_BIT_VECTOR ||
        result_kind != QL_IR_TYPE_BIT_VECTOR ||
        graph->types[result_type].bit_width >=
            graph->types[first_type].bit_width) {
      return instruction_type_error(id, "integer truncation", error);
    }
    break;
  case QL_IR_OPCODE_BITCAST:
    REQUIRE_COUNTS(1u, 1u);
    REQUIRE_PURE();
    if (first_type == result_type ||
        (first_kind != QL_IR_TYPE_BIT_VECTOR &&
         first_kind != QL_IR_TYPE_FLOAT) ||
        (result_kind != QL_IR_TYPE_BIT_VECTOR &&
         result_kind != QL_IR_TYPE_FLOAT) ||
        graph->types[first_type].bit_width !=
            graph->types[result_type].bit_width) {
      return instruction_type_error(id, "bitcast", error);
    }
    break;
  case QL_IR_OPCODE_PTR_TO_BV:
    REQUIRE_COUNTS(1u, 1u);
    REQUIRE_PURE();
    if (first_kind != QL_IR_TYPE_POINTER ||
        result_kind != QL_IR_TYPE_BIT_VECTOR ||
        graph->types[first_type].bit_width !=
            graph->types[result_type].bit_width) {
      return instruction_type_error(id, "pointer to bit-vector", error);
    }
    break;
  case QL_IR_OPCODE_BV_TO_PTR:
    REQUIRE_COUNTS(1u, 1u);
    REQUIRE_PURE();
    if (first_kind != QL_IR_TYPE_BIT_VECTOR ||
        result_kind != QL_IR_TYPE_POINTER ||
        graph->types[first_type].bit_width !=
            graph->types[result_type].bit_width) {
      return instruction_type_error(id, "bit-vector to pointer", error);
    }
    break;
  case QL_IR_OPCODE_PTR_ADD:
    REQUIRE_COUNTS(2u, 1u);
    REQUIRE_PURE();
    if (first_kind != QL_IR_TYPE_POINTER ||
        value_kind(graph, instruction->operands[1]) != QL_IR_TYPE_BIT_VECTOR ||
        result_type != first_type) {
      return instruction_type_error(id, "pointer addition", error);
    }
    break;
  case QL_IR_OPCODE_LOAD:
    REQUIRE_COUNTS(2u, 1u);
    if (first_kind != QL_IR_TYPE_MEMORY ||
        value_kind(graph, instruction->operands[1]) != QL_IR_TYPE_POINTER ||
        graph->types[graph->values[instruction->operands[1]].type]
                .element_type != result_type ||
        (instruction->effects & QL_IR_EFFECT_MEMORY) == 0u) {
      return instruction_type_error(id, "load", error);
    }
    break;
  case QL_IR_OPCODE_STORE:
    REQUIRE_COUNTS(3u, 1u);
    if (first_kind != QL_IR_TYPE_MEMORY ||
        value_kind(graph, instruction->operands[1]) != QL_IR_TYPE_POINTER ||
        graph->types[graph->values[instruction->operands[1]].type]
                .element_type != graph->values[instruction->operands[2]].type ||
        result_type != first_type ||
        (instruction->effects & QL_IR_EFFECT_MEMORY) == 0u) {
      return instruction_type_error(id, "store", error);
    }
    break;
  case QL_IR_OPCODE_CALL:
    if (instruction->symbol_size == 0u ||
        (instruction->effects & QL_IR_EFFECT_CALL) == 0u) {
      return instruction_type_error(id, "call", error);
    }
    break;
  case QL_IR_OPCODE_TRACE_APPEND:
    if (instruction->operand_count == 0u || instruction->result_count != 1u ||
        first_kind != QL_IR_TYPE_EVENT_TRACE || result_type != first_type ||
        instruction->effects == 0u) {
      return instruction_type_error(id, "event trace append", error);
    }
    break;
  case QL_IR_OPCODE_ASSUME:
    REQUIRE_COUNTS(1u, 0u);
    REQUIRE_PURE();
    if (first_kind != QL_IR_TYPE_BOOL) {
      return instruction_type_error(id, "assume", error);
    }
    break;
  case QL_IR_OPCODE_MEMORY_IMAGE:
    REQUIRE_COUNTS(2u, 1u);
    /* It reads no memory the program could observe and writes none: it
       asks a question about a memory value it was handed. Declaring an
       effect would make it an access, and an access needs a guard. */
    REQUIRE_PURE();
    if (first_kind != QL_IR_TYPE_MEMORY ||
        value_kind(graph, instruction->operands[1]) != QL_IR_TYPE_POINTER ||
        result_kind != QL_IR_TYPE_BOOL || instruction->image_size == 0u) {
      return instruction_type_error(id, "memory image", error);
    }
    break;
  case QL_IR_OPCODE_UB_GUARD:
    REQUIRE_COUNTS(1u, 0u);
    if (first_kind != QL_IR_TYPE_BOOL ||
        instruction->effects != QL_IR_EFFECT_UNDEFINED_BEHAVIOR) {
      return instruction_type_error(id, "UB guard", error);
    }
    break;
  case QL_IR_OPCODE_FNEG:
    REQUIRE_COUNTS(1u, 1u);
    REQUIRE_PURE();
    if (first_kind != QL_IR_TYPE_FLOAT || first_type != result_type) {
      return instruction_type_error(id, "floating unary", error);
    }
    break;
  case QL_IR_OPCODE_FADD:
  case QL_IR_OPCODE_FSUB:
  case QL_IR_OPCODE_FMUL:
  case QL_IR_OPCODE_FDIV:
  case QL_IR_OPCODE_FREM:
    REQUIRE_COUNTS(2u, 1u);
    REQUIRE_PURE();
    if (first_kind != QL_IR_TYPE_FLOAT ||
        !all_operands_same_type(graph, instruction, first_type) ||
        result_type != first_type) {
      return instruction_type_error(id, "floating binary", error);
    }
    break;
  case QL_IR_OPCODE_FOEQ:
  case QL_IR_OPCODE_FONE:
  case QL_IR_OPCODE_FOLT:
  case QL_IR_OPCODE_FOLE:
    REQUIRE_COUNTS(2u, 1u);
    REQUIRE_PURE();
    if (first_kind != QL_IR_TYPE_FLOAT ||
        !all_operands_same_type(graph, instruction, first_type) ||
        result_kind != QL_IR_TYPE_BOOL) {
      return instruction_type_error(id, "floating comparison", error);
    }
    break;
  case QL_IR_OPCODE_FP_TO_SBV:
  case QL_IR_OPCODE_FP_TO_UBV:
    REQUIRE_COUNTS(1u, 1u);
    REQUIRE_PURE();
    if (first_kind != QL_IR_TYPE_FLOAT ||
        result_kind != QL_IR_TYPE_BIT_VECTOR) {
      return instruction_type_error(id, "float to integer", error);
    }
    break;
  case QL_IR_OPCODE_SBV_TO_FP:
  case QL_IR_OPCODE_UBV_TO_FP:
    REQUIRE_COUNTS(1u, 1u);
    REQUIRE_PURE();
    if (first_kind != QL_IR_TYPE_BIT_VECTOR ||
        result_kind != QL_IR_TYPE_FLOAT) {
      return instruction_type_error(id, "integer to float", error);
    }
    break;
  case QL_IR_OPCODE_FP_EXT:
    REQUIRE_COUNTS(1u, 1u);
    REQUIRE_PURE();
    if (first_kind != QL_IR_TYPE_FLOAT || result_kind != QL_IR_TYPE_FLOAT ||
        graph->types[result_type].bit_width <=
            graph->types[first_type].bit_width) {
      return instruction_type_error(id, "float extension", error);
    }
    break;
  case QL_IR_OPCODE_FP_TRUNC:
    REQUIRE_COUNTS(1u, 1u);
    REQUIRE_PURE();
    if (first_kind != QL_IR_TYPE_FLOAT || result_kind != QL_IR_TYPE_FLOAT ||
        graph->types[result_type].bit_width >=
            graph->types[first_type].bit_width) {
      return instruction_type_error(id, "float truncation", error);
    }
    break;
  default:
    /* Extension opcodes retain reference and result typing, while their
       semantic rules belong to the explicitly versioned consumer. */
    break;
  }

#undef REQUIRE_PURE
#undef REQUIRE_COUNTS
  return QL_STATUS_OK;
}

static ql_status copy_id_array(const ql_allocator *allocator,
                               const uint32_t *source, size_t count,
                               uint32_t **output, ql_error *error) {
  size_t byte_size;

  if (!checked_mul_size(count, sizeof(*source), &byte_size)) {
    ql_error_set(error, QL_STATUS_INVALID_ARGUMENT,
                 "IR ID array size overflows size_t");
    return QL_STATUS_INVALID_ARGUMENT;
  }
  return copy_bytes(allocator, source, byte_size, (void **)output, error);
}

ql_status QL_CALL ql_ir_builder_append_instruction(
    ql_ir_builder *builder, ql_ir_block_id block,
    const ql_ir_instruction_definition_v1 *definition,
    ql_ir_instruction_id *instruction_output, ql_ir_value_id *results_output,
    ql_error *error) {
  ir_graph *graph;
  ir_instruction_entry entry;
  ir_block_entry *block_entry;
  size_t new_value_count;
  size_t index;
  ql_ir_instruction_id instruction_id;
  ql_status status;

  if (builder == NULL || instruction_output == NULL || definition == NULL) {
    ql_error_set(error, QL_STATUS_INVALID_ARGUMENT,
                 "IR instruction requires a builder, definition, and output");
    return QL_STATUS_INVALID_ARGUMENT;
  }
  *instruction_output = QL_IR_INVALID_INSTRUCTION_ID;
  graph = &builder->graph;
  if (definition->struct_size < sizeof(*definition)) {
    ql_error_set(error, QL_STATUS_ABI_MISMATCH,
                 "IR instruction definition v1 has an invalid size");
    return QL_STATUS_ABI_MISMATCH;
  }
  if (block >= graph->block_count ||
      graph->blocks[block].terminator.present != 0u) {
    ql_error_set(error, QL_STATUS_INVALID_ARGUMENT,
                 "IR instruction block is missing or already terminated");
    return QL_STATUS_INVALID_ARGUMENT;
  }
  if ((definition->operand_count != 0u && definition->operands == NULL) ||
      (definition->block_operand_count != 0u &&
       definition->block_operands == NULL) ||
      (definition->result_count != 0u &&
       (definition->result_types == NULL || results_output == NULL)) ||
      (definition->symbol_size != 0u && definition->symbol == NULL) ||
      (definition->image_size != 0u && definition->image == NULL)) {
    ql_error_set(error, QL_STATUS_INVALID_ARGUMENT,
                 "IR instruction pointer and count fields disagree");
    return QL_STATUS_INVALID_ARGUMENT;
  }
  if (definition->operand_count > (size_t)UINT32_MAX ||
      definition->block_operand_count > (size_t)UINT32_MAX ||
      definition->result_count > (size_t)UINT32_MAX ||
      graph->instruction_count >= (size_t)UINT32_MAX ||
      !checked_add_size(graph->value_count, definition->result_count,
                        &new_value_count) ||
      new_value_count > (size_t)UINT32_MAX) {
    ql_error_set(error, QL_STATUS_INVALID_ARGUMENT,
                 "IR instruction sizes exhaust the v1 ID space");
    return QL_STATUS_INVALID_ARGUMENT;
  }
  for (index = 0u; index < definition->operand_count; ++index) {
    if (definition->operands[index] >= graph->value_count) {
      ql_error_set(error, QL_STATUS_INVALID_ARGUMENT,
                   "IR instruction references missing value %u",
                   definition->operands[index]);
      return QL_STATUS_INVALID_ARGUMENT;
    }
  }
  for (index = 0u; index < definition->block_operand_count; ++index) {
    if (definition->block_operands[index] >= graph->block_count) {
      ql_error_set(error, QL_STATUS_INVALID_ARGUMENT,
                   "IR instruction references missing block %u",
                   definition->block_operands[index]);
      return QL_STATUS_INVALID_ARGUMENT;
    }
  }
  for (index = 0u; index < definition->result_count; ++index) {
    status =
        validate_value_type(graph, definition->result_types[index], 1, error);
    if (status != QL_STATUS_OK) {
      return status;
    }
  }

  memset(&entry, 0, sizeof(entry));
  entry.block = block;
  entry.opcode = definition->opcode;
  entry.flags = definition->flags;
  entry.effects = definition->effects;
  entry.operand_count = definition->operand_count;
  entry.block_operand_count = definition->block_operand_count;
  entry.result_count = definition->result_count;
  entry.immediate = definition->immediate;
  entry.symbol_size = definition->symbol_size;
  entry.image_size = definition->image_size;
  status = copy_id_array(&graph->allocator, definition->operands,
                         definition->operand_count,
                         (uint32_t **)&entry.operands, error);
  if (status == QL_STATUS_OK) {
    status = copy_id_array(&graph->allocator, definition->block_operands,
                           definition->block_operand_count,
                           (uint32_t **)&entry.block_operands, error);
  }
  if (status == QL_STATUS_OK) {
    size_t result_byte_size;
    if (!checked_mul_size(definition->result_count, sizeof(*entry.results),
                          &result_byte_size)) {
      status = QL_STATUS_INVALID_ARGUMENT;
      ql_error_set(error, status, "IR result array size overflows size_t");
    } else {
      status = copy_bytes(&graph->allocator, NULL, 0u, (void **)&entry.results,
                          error);
      if (status == QL_STATUS_OK && result_byte_size != 0u) {
        entry.results = graph->allocator.allocate(graph->allocator.user_data,
                                                  result_byte_size);
        if (entry.results == NULL) {
          status = QL_STATUS_OUT_OF_MEMORY;
          ql_error_set(error, status, NULL);
        }
      }
    }
  }
  if (status == QL_STATUS_OK) {
    status = copy_text(&graph->allocator, definition->symbol,
                       definition->symbol_size, &entry.symbol, error);
  }
  if (status == QL_STATUS_OK) {
    status = copy_bytes(&graph->allocator, definition->image,
                        definition->image_size, &entry.image, error);
  }
  if (status != QL_STATUS_OK) {
    free_instruction(&graph->allocator, &entry);
    return status;
  }
  status =
      reserve_array(&graph->allocator, (void **)&graph->instructions,
                    &graph->instruction_capacity, graph->instruction_count + 1u,
                    sizeof(*graph->instructions), error);
  if (status == QL_STATUS_OK) {
    status = reserve_array(&graph->allocator, (void **)&graph->values,
                           &graph->value_capacity, new_value_count,
                           sizeof(*graph->values), error);
  }
  block_entry = &graph->blocks[block];
  if (status == QL_STATUS_OK) {
    status = reserve_array(
        &graph->allocator, (void **)&block_entry->instructions,
        &block_entry->instruction_capacity, block_entry->instruction_count + 1u,
        sizeof(*block_entry->instructions), error);
  }
  if (status != QL_STATUS_OK) {
    free_instruction(&graph->allocator, &entry);
    return status;
  }

  instruction_id = (ql_ir_instruction_id)graph->instruction_count;
  for (index = 0u; index < definition->result_count; ++index) {
    ql_ir_value_id value_id = (ql_ir_value_id)(graph->value_count + index);
    ir_value_entry *value = &graph->values[graph->value_count + index];
    memset(value, 0, sizeof(*value));
    value->type = definition->result_types[index];
    value->definition_kind = QL_IR_VALUE_INSTRUCTION_RESULT;
    value->instruction = instruction_id;
    value->result_index = (uint32_t)index;
    entry.results[index] = value_id;
  }
  graph->instructions[graph->instruction_count] = entry;
  block_entry->instructions[block_entry->instruction_count] = instruction_id;
  ++block_entry->instruction_count;
  ++graph->instruction_count;
  graph->value_count = new_value_count;
  graph->parameters_are_sealed = 1u;

  status = validate_instruction_shape(graph, instruction_id, error);
  if (status != QL_STATUS_OK) {
    --block_entry->instruction_count;
    --graph->instruction_count;
    graph->value_count -= definition->result_count;
    free_instruction(&graph->allocator,
                     &graph->instructions[graph->instruction_count]);
    return status;
  }
  for (index = 0u; index < definition->result_count; ++index) {
    results_output[index] = entry.results[index];
  }
  *instruction_output = instruction_id;
  ql_error_clear(error);
  return QL_STATUS_OK;
}

ql_status QL_CALL ql_ir_builder_append_phi_incoming(
    ql_ir_builder *builder, ql_ir_instruction_id instruction,
    ql_ir_value_id value, ql_ir_block_id block, ql_error *error) {
  ir_graph *graph;
  ir_instruction_entry *entry;
  ql_ir_value_id *operands;
  ql_ir_block_id *blocks;
  size_t count;
  size_t byte_size;
  size_t index;

  if (builder == NULL) {
    ql_error_set(error, QL_STATUS_INVALID_ARGUMENT, "IR builder is required");
    return QL_STATUS_INVALID_ARGUMENT;
  }
  graph = &builder->graph;
  if (instruction >= graph->instruction_count ||
      graph->instructions[instruction].opcode != QL_IR_OPCODE_PHI) {
    ql_error_set(error, QL_STATUS_INVALID_ARGUMENT,
                 "IR instruction %u is not a PHI", instruction);
    return QL_STATUS_INVALID_ARGUMENT;
  }
  entry = &graph->instructions[instruction];
  if (value >= graph->value_count || block >= graph->block_count) {
    ql_error_set(error, QL_STATUS_INVALID_ARGUMENT,
                 "IR PHI incoming references a missing value or block");
    return QL_STATUS_INVALID_ARGUMENT;
  }
  if (entry->result_count != 1u ||
      graph->values[value].type != graph->values[entry->results[0]].type) {
    ql_error_set(error, QL_STATUS_TYPE_MISMATCH,
                 "IR PHI incoming value has the wrong type");
    return QL_STATUS_TYPE_MISMATCH;
  }
  for (index = 0u; index < entry->block_operand_count; ++index) {
    if (entry->block_operands[index] == block) {
      ql_error_set(error, QL_STATUS_ALREADY_EXISTS,
                   "IR PHI already has an incoming edge from block %u", block);
      return QL_STATUS_ALREADY_EXISTS;
    }
  }
  if (entry->operand_count != entry->block_operand_count ||
      entry->operand_count >= (size_t)UINT32_MAX ||
      !checked_add_size(entry->operand_count, 1u, &count) ||
      !checked_mul_size(count, sizeof(*operands), &byte_size)) {
    ql_error_set(error, QL_STATUS_INVALID_ARGUMENT,
                 "IR PHI incoming list exhausts the v1 ID space");
    return QL_STATUS_INVALID_ARGUMENT;
  }
  operands = graph->allocator.allocate(graph->allocator.user_data, byte_size);
  blocks = graph->allocator.allocate(graph->allocator.user_data,
                                     count * sizeof(*blocks));
  if (operands == NULL || blocks == NULL) {
    graph->allocator.deallocate(graph->allocator.user_data, operands);
    graph->allocator.deallocate(graph->allocator.user_data, blocks);
    ql_error_set(error, QL_STATUS_OUT_OF_MEMORY, NULL);
    return QL_STATUS_OUT_OF_MEMORY;
  }
  if (entry->operand_count != 0u) {
    memcpy(operands, entry->operands, entry->operand_count * sizeof(*operands));
    memcpy(blocks, entry->block_operands,
           entry->block_operand_count * sizeof(*blocks));
  }
  operands[count - 1u] = value;
  blocks[count - 1u] = block;
  graph->allocator.deallocate(graph->allocator.user_data, entry->operands);
  graph->allocator.deallocate(graph->allocator.user_data,
                              entry->block_operands);
  entry->operands = operands;
  entry->block_operands = blocks;
  entry->operand_count = count;
  entry->block_operand_count = count;
  ql_error_clear(error);
  return QL_STATUS_OK;
}

static ql_status validate_optional_state_value(const ir_graph *graph,
                                               ql_ir_value_id value,
                                               ql_ir_type_kind required,
                                               const char *name,
                                               ql_error *error) {
  if (value == QL_IR_INVALID_VALUE_ID) {
    return QL_STATUS_OK;
  }
  if (value >= graph->value_count || value_kind(graph, value) != required) {
    ql_error_set(error, QL_STATUS_TYPE_MISMATCH,
                 "IR terminator has an invalid %s value", name);
    return QL_STATUS_TYPE_MISMATCH;
  }
  return QL_STATUS_OK;
}

static ql_status
validate_terminator_shape(const ir_graph *graph, ql_ir_block_id block,
                          const ir_terminator_entry *terminator,
                          ql_error *error) {
  ql_ir_type_kind return_kind;
  ql_status status;

  if (terminator->present == 0u) {
    ql_error_set(error, QL_STATUS_INVALID_ARGUMENT,
                 "IR block %u has no terminator", block);
    return QL_STATUS_INVALID_ARGUMENT;
  }
  status = validate_optional_state_value(graph, terminator->memory,
                                         QL_IR_TYPE_MEMORY, "memory", error);
  if (status != QL_STATUS_OK) {
    return status;
  }
  status = validate_optional_state_value(graph, terminator->event_trace,
                                         QL_IR_TYPE_EVENT_TRACE, "event trace",
                                         error);
  if (status != QL_STATUS_OK) {
    return status;
  }
  switch (terminator->kind) {
  case QL_IR_TERMINATOR_RETURN:
    if (terminator->condition != QL_IR_INVALID_VALUE_ID ||
        terminator->target != QL_IR_INVALID_BLOCK_ID ||
        terminator->false_target != QL_IR_INVALID_BLOCK_ID) {
      break;
    }
    return_kind = graph->types[graph->return_type].kind;
    if (return_kind == QL_IR_TYPE_VOID) {
      if (terminator->return_value == QL_IR_INVALID_VALUE_ID) {
        return QL_STATUS_OK;
      }
    } else if (terminator->return_value < graph->value_count &&
               graph->values[terminator->return_value].type ==
                   graph->return_type) {
      return QL_STATUS_OK;
    }
    break;
  case QL_IR_TERMINATOR_BRANCH:
    if (terminator->target < graph->block_count &&
        terminator->false_target == QL_IR_INVALID_BLOCK_ID &&
        terminator->condition == QL_IR_INVALID_VALUE_ID &&
        terminator->return_value == QL_IR_INVALID_VALUE_ID &&
        terminator->memory == QL_IR_INVALID_VALUE_ID &&
        terminator->event_trace == QL_IR_INVALID_VALUE_ID) {
      return QL_STATUS_OK;
    }
    break;
  case QL_IR_TERMINATOR_COND_BRANCH:
    if (terminator->condition < graph->value_count &&
        value_kind(graph, terminator->condition) == QL_IR_TYPE_BOOL &&
        terminator->target < graph->block_count &&
        terminator->false_target < graph->block_count &&
        terminator->target != terminator->false_target &&
        terminator->return_value == QL_IR_INVALID_VALUE_ID &&
        terminator->memory == QL_IR_INVALID_VALUE_ID &&
        terminator->event_trace == QL_IR_INVALID_VALUE_ID) {
      return QL_STATUS_OK;
    }
    break;
  case QL_IR_TERMINATOR_TRAP:
  case QL_IR_TERMINATOR_UNDEFINED_BEHAVIOR:
  case QL_IR_TERMINATOR_DIVERGE:
    if (terminator->condition == QL_IR_INVALID_VALUE_ID &&
        terminator->return_value == QL_IR_INVALID_VALUE_ID &&
        terminator->target == QL_IR_INVALID_BLOCK_ID &&
        terminator->false_target == QL_IR_INVALID_BLOCK_ID) {
      return QL_STATUS_OK;
    }
    break;
  case QL_IR_TERMINATOR_TERMINATE:
    if (terminator->condition == QL_IR_INVALID_VALUE_ID &&
        terminator->target == QL_IR_INVALID_BLOCK_ID &&
        terminator->false_target == QL_IR_INVALID_BLOCK_ID &&
        (terminator->return_value == QL_IR_INVALID_VALUE_ID ||
         (terminator->return_value < graph->value_count &&
          (value_kind(graph, terminator->return_value) == QL_IR_TYPE_BOOL ||
           value_kind(graph, terminator->return_value) ==
               QL_IR_TYPE_BIT_VECTOR)))) {
      return QL_STATUS_OK;
    }
    break;
  default:
    ql_error_set(error, QL_STATUS_INVALID_ARGUMENT,
                 "IR block %u has unknown terminator %u", block,
                 (unsigned)terminator->kind);
    return QL_STATUS_INVALID_ARGUMENT;
  }
  ql_error_set(error, QL_STATUS_TYPE_MISMATCH,
               "IR block %u has invalid terminator fields", block);
  return QL_STATUS_TYPE_MISMATCH;
}

ql_status QL_CALL ql_ir_builder_set_terminator(
    ql_ir_builder *builder, ql_ir_block_id block,
    const ql_ir_terminator_definition_v1 *definition, ql_error *error) {
  ir_terminator_entry entry;
  ql_status status;

  if (builder == NULL || definition == NULL ||
      definition->struct_size < sizeof(*definition)) {
    ql_error_set(error, QL_STATUS_ABI_MISMATCH,
                 "IR terminator definition v1 has an invalid size");
    return QL_STATUS_ABI_MISMATCH;
  }
  if (builder->graph.function_is_set == 0u) {
    ql_error_set(error, QL_STATUS_INVALID_ARGUMENT,
                 "IR function metadata must precede terminators");
    return QL_STATUS_INVALID_ARGUMENT;
  }
  if (block >= builder->graph.block_count) {
    ql_error_set(error, QL_STATUS_INVALID_ARGUMENT,
                 "IR terminator references missing block %u", block);
    return QL_STATUS_INVALID_ARGUMENT;
  }
  if (builder->graph.blocks[block].terminator.present != 0u) {
    ql_error_set(error, QL_STATUS_ALREADY_EXISTS,
                 "IR block %u already has a terminator", block);
    return QL_STATUS_ALREADY_EXISTS;
  }
  memset(&entry, 0, sizeof(entry));
  entry.present = 1u;
  entry.kind = definition->kind;
  entry.condition = definition->condition;
  entry.return_value = definition->return_value;
  entry.memory = definition->memory;
  entry.event_trace = definition->event_trace;
  entry.target = definition->target;
  entry.false_target = definition->false_target;
  entry.code = definition->code;
  entry.reason_size = definition->reason_size;
  status = copy_text(&builder->graph.allocator, definition->reason,
                     definition->reason_size, &entry.reason, error);
  if (status != QL_STATUS_OK) {
    return status;
  }
  status = validate_terminator_shape(&builder->graph, block, &entry, error);
  if (status != QL_STATUS_OK) {
    builder->graph.allocator.deallocate(builder->graph.allocator.user_data,
                                        entry.reason);
    return status;
  }
  builder->graph.blocks[block].terminator = entry;
  ql_error_clear(error);
  return QL_STATUS_OK;
}

typedef struct ir_validation_scratch {
  size_t *predecessor_offsets;
  ql_ir_block_id *predecessors;
  size_t *indegree;
  size_t *queue;
  size_t *topological;
  uint8_t *reachable;
  uint8_t *instruction_seen;
  size_t *instruction_position;
  ql_ir_block_id *idom;
  size_t *dominator_depth;
  uint32_t *predecessor_marks;
  uint32_t *incoming_marks;
} ir_validation_scratch;

static void validation_scratch_dispose(const ql_allocator *allocator,
                                       ir_validation_scratch *scratch) {
  allocator->deallocate(allocator->user_data, scratch->incoming_marks);
  allocator->deallocate(allocator->user_data, scratch->predecessor_marks);
  allocator->deallocate(allocator->user_data, scratch->dominator_depth);
  allocator->deallocate(allocator->user_data, scratch->idom);
  allocator->deallocate(allocator->user_data, scratch->instruction_position);
  allocator->deallocate(allocator->user_data, scratch->instruction_seen);
  allocator->deallocate(allocator->user_data, scratch->reachable);
  allocator->deallocate(allocator->user_data, scratch->topological);
  allocator->deallocate(allocator->user_data, scratch->queue);
  allocator->deallocate(allocator->user_data, scratch->indegree);
  allocator->deallocate(allocator->user_data, scratch->predecessors);
  allocator->deallocate(allocator->user_data, scratch->predecessor_offsets);
  memset(scratch, 0, sizeof(*scratch));
}

static ql_status allocate_zeroed(const ql_allocator *allocator, size_t count,
                                 size_t item_size, void **output,
                                 ql_error *error) {
  size_t byte_size;
  void *memory;

  *output = NULL;
  if (count == 0u) {
    return QL_STATUS_OK;
  }
  if (!checked_mul_size(count, item_size, &byte_size)) {
    ql_error_set(error, QL_STATUS_INVALID_ARGUMENT,
                 "IR validation allocation size overflows size_t");
    return QL_STATUS_INVALID_ARGUMENT;
  }
  memory = allocator->allocate(allocator->user_data, byte_size);
  if (memory == NULL) {
    ql_error_set(error, QL_STATUS_OUT_OF_MEMORY, NULL);
    return QL_STATUS_OUT_OF_MEMORY;
  }
  memset(memory, 0, byte_size);
  *output = memory;
  return QL_STATUS_OK;
}

static size_t
terminator_successor_count(const ir_terminator_entry *terminator) {
  if (terminator->kind == QL_IR_TERMINATOR_BRANCH) {
    return 1u;
  }
  if (terminator->kind == QL_IR_TERMINATOR_COND_BRANCH) {
    return 2u;
  }
  return 0u;
}

static ql_ir_block_id
terminator_successor_at(const ir_terminator_entry *terminator, size_t index) {
  return index == 0u ? terminator->target : terminator->false_target;
}

static ql_status allocate_validation_scratch(const ir_graph *graph,
                                             size_t edge_count,
                                             ir_validation_scratch *scratch,
                                             ql_error *error) {
  ql_status status;
  size_t block_offsets_count;

  memset(scratch, 0, sizeof(*scratch));
  if (!checked_add_size(graph->block_count, 1u, &block_offsets_count)) {
    ql_error_set(error, QL_STATUS_INVALID_ARGUMENT,
                 "IR block offset count overflows size_t");
    return QL_STATUS_INVALID_ARGUMENT;
  }
#define ALLOCATE_FIELD(field_, count_, type_)                                  \
  do {                                                                         \
    status = allocate_zeroed(&graph->allocator, (count_), sizeof(type_),       \
                             (void **)&scratch->field_, error);                \
    if (status != QL_STATUS_OK) {                                              \
      goto failure;                                                            \
    }                                                                          \
  } while (0)
  ALLOCATE_FIELD(predecessor_offsets, block_offsets_count, size_t);
  ALLOCATE_FIELD(predecessors, edge_count, ql_ir_block_id);
  ALLOCATE_FIELD(indegree, graph->block_count, size_t);
  ALLOCATE_FIELD(queue, graph->block_count, size_t);
  ALLOCATE_FIELD(topological, graph->block_count, size_t);
  ALLOCATE_FIELD(reachable, graph->block_count, uint8_t);
  ALLOCATE_FIELD(instruction_seen, graph->instruction_count, uint8_t);
  ALLOCATE_FIELD(instruction_position, graph->instruction_count, size_t);
  ALLOCATE_FIELD(idom, graph->block_count, ql_ir_block_id);
  ALLOCATE_FIELD(dominator_depth, graph->block_count, size_t);
  ALLOCATE_FIELD(predecessor_marks, graph->block_count, uint32_t);
  ALLOCATE_FIELD(incoming_marks, graph->block_count, uint32_t);
#undef ALLOCATE_FIELD
  return QL_STATUS_OK;

failure:
#undef ALLOCATE_FIELD
  validation_scratch_dispose(&graph->allocator, scratch);
  return status;
}

static ql_ir_block_id dominator_lca(ql_ir_block_id left, ql_ir_block_id right,
                                    const ql_ir_block_id *idom,
                                    const size_t *depth) {
  while (depth[left] > depth[right]) {
    left = idom[left];
  }
  while (depth[right] > depth[left]) {
    right = idom[right];
  }
  while (left != right) {
    left = idom[left];
    right = idom[right];
  }
  return left;
}

static ql_ir_block_id dominator_intersect_rpo(ql_ir_block_id left,
                                              ql_ir_block_id right,
                                              const ql_ir_block_id *idom,
                                              const size_t *rpo_index) {
  while (left != right) {
    while (rpo_index[left] > rpo_index[right]) {
      left = idom[left];
    }
    while (rpo_index[right] > rpo_index[left]) {
      right = idom[right];
    }
  }
  return left;
}

static int block_dominates(ql_ir_block_id definition, ql_ir_block_id use,
                           const ql_ir_block_id *idom, const size_t *depth) {
  while (depth[use] > depth[definition]) {
    use = idom[use];
  }
  return definition == use;
}

static int value_dominates_position(const ir_graph *graph, ql_ir_value_id value,
                                    ql_ir_block_id block, size_t position,
                                    const ir_validation_scratch *scratch) {
  const ir_value_entry *entry = &graph->values[value];
  const ir_instruction_entry *definition;

  if (entry->definition_kind == QL_IR_VALUE_PARAMETER ||
      entry->definition_kind == QL_IR_VALUE_CONSTANT) {
    return 1;
  }
  definition = &graph->instructions[entry->instruction];
  if (definition->block == block) {
    return scratch->instruction_position[entry->instruction] < position;
  }
  return block_dominates(definition->block, block, scratch->idom,
                         scratch->dominator_depth);
}

static ql_status validate_instruction_dominance(
    const ir_graph *graph, ql_ir_instruction_id instruction_id,
    const ir_validation_scratch *scratch, ql_error *error) {
  const ir_instruction_entry *instruction =
      &graph->instructions[instruction_id];
  ql_ir_block_id block = instruction->block;
  size_t position = scratch->instruction_position[instruction_id];
  size_t index;
  uint32_t block_mark = block + 1u;
  uint32_t phi_mark = instruction_id + 1u;

  if (instruction->opcode == QL_IR_OPCODE_PHI) {
    size_t predecessor_count = scratch->predecessor_offsets[block + 1u] -
                               scratch->predecessor_offsets[block];
    if (instruction->block_operand_count != predecessor_count) {
      ql_error_set(error, QL_STATUS_INVALID_ARGUMENT,
                   "IR PHI %u does not cover every predecessor",
                   instruction_id);
      return QL_STATUS_INVALID_ARGUMENT;
    }
    for (index = 0u; index < instruction->block_operand_count; ++index) {
      ql_ir_block_id incoming_block = instruction->block_operands[index];
      ql_ir_value_id incoming_value = instruction->operands[index];
      if (scratch->predecessor_marks[incoming_block] != block_mark ||
          scratch->incoming_marks[incoming_block] == phi_mark) {
        ql_error_set(error, QL_STATUS_INVALID_ARGUMENT,
                     "IR PHI %u has a missing or duplicate incoming edge",
                     instruction_id);
        return QL_STATUS_INVALID_ARGUMENT;
      }
      scratch->incoming_marks[incoming_block] = phi_mark;
      if (!value_dominates_position(
              graph, incoming_value, incoming_block,
              graph->blocks[incoming_block].instruction_count, scratch)) {
        ql_error_set(error, QL_STATUS_INVALID_ARGUMENT,
                     "IR PHI %u incoming value does not dominate edge %u",
                     instruction_id, incoming_block);
        return QL_STATUS_INVALID_ARGUMENT;
      }
    }
    return QL_STATUS_OK;
  }
  for (index = 0u; index < instruction->operand_count; ++index) {
    if (!value_dominates_position(graph, instruction->operands[index], block,
                                  position, scratch)) {
      ql_error_set(error, QL_STATUS_INVALID_ARGUMENT,
                   "IR instruction %u uses non-dominating value %u",
                   instruction_id, instruction->operands[index]);
      return QL_STATUS_INVALID_ARGUMENT;
    }
  }
  return QL_STATUS_OK;
}

static ql_status
validate_terminator_dominance(const ir_graph *graph, ql_ir_block_id block,
                              const ir_validation_scratch *scratch,
                              ql_error *error) {
  const ir_terminator_entry *terminator = &graph->blocks[block].terminator;
  const ql_ir_value_id values[4] = {
      terminator->condition, terminator->return_value, terminator->memory,
      terminator->event_trace};
  size_t index;

  for (index = 0u; index < 4u; ++index) {
    if (values[index] != QL_IR_INVALID_VALUE_ID &&
        !value_dominates_position(graph, values[index], block,
                                  graph->blocks[block].instruction_count,
                                  scratch)) {
      ql_error_set(error, QL_STATUS_INVALID_ARGUMENT,
                   "IR block %u terminator uses non-dominating value %u", block,
                   values[index]);
      return QL_STATUS_INVALID_ARGUMENT;
    }
  }
  return QL_STATUS_OK;
}

static ql_status graph_validate(const ir_graph *graph, ql_error *error) {
  ir_validation_scratch scratch;
  size_t edge_count = 0u;
  size_t index;
  size_t block_index;
  size_t instruction_index;
  size_t cursor;
  size_t queue_begin;
  size_t queue_end;
  size_t topo_count;
  ql_status status;

  if ((graph->cfg_kind != QL_IR_CFG_ACYCLIC &&
       graph->cfg_kind != QL_IR_CFG_CYCLIC) ||
      graph->function_is_set == 0u || graph->function_name == NULL ||
      graph->function_name_size == 0u ||
      graph->return_type >= graph->type_count || graph->block_count == 0u ||
      graph->entry_block >= graph->block_count ||
      graph->type_count >= (size_t)UINT32_MAX ||
      graph->value_count >= (size_t)UINT32_MAX ||
      graph->block_count >= (size_t)UINT32_MAX ||
      graph->instruction_count >= (size_t)UINT32_MAX) {
    ql_error_set(error, QL_STATUS_INVALID_ARGUMENT,
                 "IR graph metadata is incomplete or exceeds schema v1");
    return QL_STATUS_INVALID_ARGUMENT;
  }
  if (memchr(graph->function_name, '\0', graph->function_name_size) != NULL) {
    ql_error_set(error, QL_STATUS_INVALID_ARGUMENT,
                 "IR function name contains an embedded NUL");
    return QL_STATUS_INVALID_ARGUMENT;
  }
  for (index = 0u; index < graph->type_count; ++index) {
    status = validate_stored_type(graph, index, error);
    if (status != QL_STATUS_OK) {
      return status;
    }
    if (graph->types[index].kind == QL_IR_TYPE_POINTER &&
        graph->types[index].element_type >= index) {
      ql_error_set(error, QL_STATUS_INVALID_ARGUMENT,
                   "IR pointer type %zu has a forward element reference",
                   index);
      return QL_STATUS_INVALID_ARGUMENT;
    }
  }
  for (index = 0u; index < graph->value_count; ++index) {
    const ir_value_entry *value = &graph->values[index];
    status = validate_value_type(graph, value->type, 1, error);
    if (status != QL_STATUS_OK) {
      return status;
    }
    switch (value->definition_kind) {
    case QL_IR_VALUE_PARAMETER:
      if (value->constant_data != NULL || value->constant_size != 0u ||
          value->instruction != QL_IR_INVALID_INSTRUCTION_ID) {
        ql_error_set(error, QL_STATUS_INVALID_ARGUMENT,
                     "IR parameter %zu has invalid definition fields", index);
        return QL_STATUS_INVALID_ARGUMENT;
      }
      break;
    case QL_IR_VALUE_CONSTANT:
      if (value->name != NULL || value->name_size != 0u ||
          value->instruction != QL_IR_INVALID_INSTRUCTION_ID) {
        ql_error_set(error, QL_STATUS_INVALID_ARGUMENT,
                     "IR constant %zu has invalid definition fields", index);
        return QL_STATUS_INVALID_ARGUMENT;
      }
      status = validate_constant(graph, value->type, value->constant_data,
                                 value->constant_size, error);
      if (status != QL_STATUS_OK) {
        return status;
      }
      break;
    case QL_IR_VALUE_INSTRUCTION_RESULT:
      if (value->name != NULL || value->name_size != 0u ||
          value->constant_data != NULL || value->constant_size != 0u ||
          value->instruction >= graph->instruction_count) {
        ql_error_set(error, QL_STATUS_INVALID_ARGUMENT,
                     "IR result value %zu has invalid definition fields",
                     index);
        return QL_STATUS_INVALID_ARGUMENT;
      }
      break;
    default:
      ql_error_set(error, QL_STATUS_INVALID_ARGUMENT,
                   "IR value %zu has unknown definition kind", index);
      return QL_STATUS_INVALID_ARGUMENT;
    }
  }
  for (index = 0u; index < graph->instruction_count; ++index) {
    if (graph->instructions[index].block >= graph->block_count) {
      ql_error_set(error, QL_STATUS_INVALID_ARGUMENT,
                   "IR instruction %zu references missing block", index);
      return QL_STATUS_INVALID_ARGUMENT;
    }
    status =
        validate_instruction_shape(graph, (ql_ir_instruction_id)index, error);
    if (status != QL_STATUS_OK) {
      return status;
    }
  }
  for (block_index = 0u; block_index < graph->block_count; ++block_index) {
    const ir_block_entry *block = &graph->blocks[block_index];
    size_t successor_count;
    status = validate_terminator_shape(graph, (ql_ir_block_id)block_index,
                                       &block->terminator, error);
    if (status != QL_STATUS_OK) {
      return status;
    }
    successor_count = terminator_successor_count(&block->terminator);
    if (!checked_add_size(edge_count, successor_count, &edge_count)) {
      ql_error_set(error, QL_STATUS_INVALID_ARGUMENT,
                   "IR CFG edge count overflows size_t");
      return QL_STATUS_INVALID_ARGUMENT;
    }
  }
  status = allocate_validation_scratch(graph, edge_count, &scratch, error);
  if (status != QL_STATUS_OK) {
    return status;
  }

  for (block_index = 0u; block_index < graph->block_count; ++block_index) {
    const ir_block_entry *block = &graph->blocks[block_index];
    uint32_t saw_non_phi = 0u;
    for (index = 0u; index < block->instruction_count; ++index) {
      ql_ir_instruction_id instruction = block->instructions[index];
      if (instruction >= graph->instruction_count ||
          scratch.instruction_seen[instruction] != 0u ||
          graph->instructions[instruction].block != block_index) {
        ql_error_set(error, QL_STATUS_INVALID_ARGUMENT,
                     "IR block %zu has an invalid instruction list",
                     block_index);
        status = QL_STATUS_INVALID_ARGUMENT;
        goto cleanup;
      }
      if (graph->instructions[instruction].opcode == QL_IR_OPCODE_PHI) {
        if (saw_non_phi != 0u) {
          ql_error_set(error, QL_STATUS_INVALID_ARGUMENT,
                       "IR block %zu has a PHI after a non-PHI", block_index);
          status = QL_STATUS_INVALID_ARGUMENT;
          goto cleanup;
        }
      } else {
        saw_non_phi = 1u;
      }
      scratch.instruction_seen[instruction] = 1u;
      scratch.instruction_position[instruction] = index;
    }
  }
  for (instruction_index = 0u; instruction_index < graph->instruction_count;
       ++instruction_index) {
    if (scratch.instruction_seen[instruction_index] == 0u) {
      ql_error_set(error, QL_STATUS_INVALID_ARGUMENT,
                   "IR instruction %zu is not owned by one block",
                   instruction_index);
      status = QL_STATUS_INVALID_ARGUMENT;
      goto cleanup;
    }
  }

  for (block_index = 0u; block_index < graph->block_count; ++block_index) {
    const ir_terminator_entry *terminator =
        &graph->blocks[block_index].terminator;
    size_t successor_count = terminator_successor_count(terminator);
    for (index = 0u; index < successor_count; ++index) {
      ql_ir_block_id successor = terminator_successor_at(terminator, index);
      ++scratch.indegree[successor];
    }
  }
  cursor = 0u;
  for (block_index = 0u; block_index < graph->block_count; ++block_index) {
    scratch.predecessor_offsets[block_index] = cursor;
    if (!checked_add_size(cursor, scratch.indegree[block_index], &cursor)) {
      ql_error_set(error, QL_STATUS_INVALID_ARGUMENT,
                   "IR predecessor table overflows size_t");
      status = QL_STATUS_INVALID_ARGUMENT;
      goto cleanup;
    }
  }
  scratch.predecessor_offsets[graph->block_count] = cursor;
  memset(scratch.queue, 0, graph->block_count * sizeof(*scratch.queue));
  for (block_index = 0u; block_index < graph->block_count; ++block_index) {
    const ir_terminator_entry *terminator =
        &graph->blocks[block_index].terminator;
    size_t successor_count = terminator_successor_count(terminator);
    for (index = 0u; index < successor_count; ++index) {
      ql_ir_block_id successor = terminator_successor_at(terminator, index);
      size_t offset =
          scratch.predecessor_offsets[successor] + scratch.queue[successor]++;
      scratch.predecessors[offset] = (ql_ir_block_id)block_index;
    }
  }

  queue_begin = 0u;
  queue_end = 0u;
  scratch.queue[queue_end++] = graph->entry_block;
  scratch.reachable[graph->entry_block] = 1u;
  while (queue_begin < queue_end) {
    ql_ir_block_id block = (ql_ir_block_id)scratch.queue[queue_begin++];
    const ir_terminator_entry *terminator = &graph->blocks[block].terminator;
    size_t successor_count = terminator_successor_count(terminator);
    for (index = 0u; index < successor_count; ++index) {
      ql_ir_block_id successor = terminator_successor_at(terminator, index);
      if (scratch.reachable[successor] == 0u) {
        scratch.reachable[successor] = 1u;
        scratch.queue[queue_end++] = successor;
      }
    }
  }
  if (queue_end != graph->block_count) {
    ql_error_set(error, QL_STATUS_INVALID_ARGUMENT,
                 "IR contains an unreachable block");
    status = QL_STATUS_INVALID_ARGUMENT;
    goto cleanup;
  }

  if (graph->cfg_kind == QL_IR_CFG_ACYCLIC) {
    queue_begin = 0u;
    queue_end = 0u;
    topo_count = 0u;
    for (block_index = 0u; block_index < graph->block_count; ++block_index) {
      if (scratch.indegree[block_index] == 0u) {
        scratch.queue[queue_end++] = block_index;
      }
    }
    while (queue_begin < queue_end) {
      ql_ir_block_id block = (ql_ir_block_id)scratch.queue[queue_begin++];
      const ir_terminator_entry *terminator = &graph->blocks[block].terminator;
      size_t successor_count = terminator_successor_count(terminator);
      scratch.topological[topo_count++] = block;
      for (index = 0u; index < successor_count; ++index) {
        ql_ir_block_id successor = terminator_successor_at(terminator, index);
        --scratch.indegree[successor];
        if (scratch.indegree[successor] == 0u) {
          scratch.queue[queue_end++] = successor;
        }
      }
    }
    if (topo_count != graph->block_count) {
      ql_error_set(error, QL_STATUS_CYCLE,
                   "acyclic IR rejects CFG cycles and back-edges");
      status = QL_STATUS_CYCLE;
      goto cleanup;
    }

    for (block_index = 0u; block_index < graph->block_count; ++block_index) {
      scratch.idom[block_index] = QL_IR_INVALID_BLOCK_ID;
    }
    scratch.idom[graph->entry_block] = graph->entry_block;
    for (index = 1u; index < topo_count; ++index) {
      ql_ir_block_id block = (ql_ir_block_id)scratch.topological[index];
      size_t begin = scratch.predecessor_offsets[block];
      size_t end = scratch.predecessor_offsets[block + 1u];
      ql_ir_block_id immediate = scratch.predecessors[begin];
      size_t predecessor_index;
      for (predecessor_index = begin + 1u; predecessor_index < end;
           ++predecessor_index) {
        immediate =
            dominator_lca(immediate, scratch.predecessors[predecessor_index],
                          scratch.idom, scratch.dominator_depth);
      }
      scratch.idom[block] = immediate;
      scratch.dominator_depth[block] = scratch.dominator_depth[immediate] + 1u;
    }
  } else {
    size_t stack_size = 0u;
    int changed;

    /* A depth-first postorder gives the reverse-postorder numbering used
       by the standard iterative immediate-dominator algorithm. Reuse the
       indegree array as each DFS frame's next-successor cursor. */
    memset(scratch.reachable, 0,
           graph->block_count * sizeof(*scratch.reachable));
    memset(scratch.indegree, 0, graph->block_count * sizeof(*scratch.indegree));
    scratch.queue[stack_size++] = graph->entry_block;
    scratch.reachable[graph->entry_block] = 1u;
    topo_count = 0u;
    while (stack_size != 0u) {
      ql_ir_block_id block = (ql_ir_block_id)scratch.queue[stack_size - 1u];
      const ir_terminator_entry *terminator = &graph->blocks[block].terminator;
      size_t successor_count = terminator_successor_count(terminator);
      if (scratch.indegree[block] < successor_count) {
        ql_ir_block_id successor =
            terminator_successor_at(terminator, scratch.indegree[block]++);
        if (scratch.reachable[successor] == 0u) {
          scratch.reachable[successor] = 1u;
          scratch.queue[stack_size++] = successor;
        }
      } else {
        scratch.topological[topo_count++] = block;
        --stack_size;
      }
    }
    for (index = 0u; index < topo_count / 2u; ++index) {
      size_t other = topo_count - index - 1u;
      size_t temporary = scratch.topological[index];
      scratch.topological[index] = scratch.topological[other];
      scratch.topological[other] = temporary;
    }
    for (index = 0u; index < topo_count; ++index) {
      scratch.dominator_depth[scratch.topological[index]] = index;
    }
    for (block_index = 0u; block_index < graph->block_count; ++block_index) {
      scratch.idom[block_index] = QL_IR_INVALID_BLOCK_ID;
    }
    scratch.idom[graph->entry_block] = graph->entry_block;
    do {
      changed = 0;
      for (index = 1u; index < topo_count; ++index) {
        ql_ir_block_id block = (ql_ir_block_id)scratch.topological[index];
        size_t begin = scratch.predecessor_offsets[block];
        size_t end = scratch.predecessor_offsets[block + 1u];
        ql_ir_block_id immediate = QL_IR_INVALID_BLOCK_ID;
        size_t predecessor_index;
        for (predecessor_index = begin; predecessor_index < end;
             ++predecessor_index) {
          ql_ir_block_id predecessor = scratch.predecessors[predecessor_index];
          if (scratch.idom[predecessor] == QL_IR_INVALID_BLOCK_ID) {
            continue;
          }
          immediate = immediate == QL_IR_INVALID_BLOCK_ID
                          ? predecessor
                          : dominator_intersect_rpo(immediate, predecessor,
                                                    scratch.idom,
                                                    scratch.dominator_depth);
        }
        if (scratch.idom[block] != immediate) {
          scratch.idom[block] = immediate;
          changed = 1;
        }
      }
    } while (changed != 0);
    memset(scratch.dominator_depth, 0,
           graph->block_count * sizeof(*scratch.dominator_depth));
    for (index = 1u; index < topo_count; ++index) {
      ql_ir_block_id block = (ql_ir_block_id)scratch.topological[index];
      ql_ir_block_id immediate = scratch.idom[block];
      if (immediate == QL_IR_INVALID_BLOCK_ID) {
        ql_error_set(error, QL_STATUS_INTERNAL_ERROR,
                     "cyclic IR dominator computation did not converge");
        status = QL_STATUS_INTERNAL_ERROR;
        goto cleanup;
      }
      scratch.dominator_depth[block] = scratch.dominator_depth[immediate] + 1u;
    }
  }
  for (block_index = 0u; block_index < graph->block_count; ++block_index) {
    uint32_t block_mark = (uint32_t)block_index + 1u;
    size_t begin = scratch.predecessor_offsets[block_index];
    size_t end = scratch.predecessor_offsets[block_index + 1u];
    for (index = begin; index < end; ++index) {
      scratch.predecessor_marks[scratch.predecessors[index]] = block_mark;
    }
    for (index = 0u; index < graph->blocks[block_index].instruction_count;
         ++index) {
      ql_ir_instruction_id instruction =
          graph->blocks[block_index].instructions[index];
      status =
          validate_instruction_dominance(graph, instruction, &scratch, error);
      if (status != QL_STATUS_OK) {
        goto cleanup;
      }
    }
    status = validate_terminator_dominance(graph, (ql_ir_block_id)block_index,
                                           &scratch, error);
    if (status != QL_STATUS_OK) {
      goto cleanup;
    }
  }
  status = QL_STATUS_OK;
  ql_error_clear(error);

cleanup:
  validation_scratch_dispose(&graph->allocator, &scratch);
  return status;
}

typedef struct ir_writer {
  const ql_allocator *allocator;
  uint8_t *data;
  size_t size;
  size_t capacity;
} ir_writer;

static ql_status writer_reserve(ir_writer *writer, size_t additional,
                                ql_error *error) {
  size_t needed;
  size_t new_capacity;
  void *new_data;

  if (!checked_add_size(writer->size, additional, &needed)) {
    ql_error_set(error, QL_STATUS_INVALID_ARGUMENT,
                 "serialized IR size overflows size_t");
    return QL_STATUS_INVALID_ARGUMENT;
  }
  if (needed <= writer->capacity) {
    return QL_STATUS_OK;
  }
  new_capacity = writer->capacity == 0u ? 1024u : writer->capacity;
  while (new_capacity < needed) {
    if (new_capacity > SIZE_MAX / 2u) {
      new_capacity = needed;
      break;
    }
    new_capacity *= 2u;
  }
  new_data = writer->allocator->reallocate(writer->allocator->user_data,
                                           writer->data, new_capacity);
  if (new_data == NULL) {
    ql_error_set(error, QL_STATUS_OUT_OF_MEMORY, NULL);
    return QL_STATUS_OUT_OF_MEMORY;
  }
  writer->data = (uint8_t *)new_data;
  writer->capacity = new_capacity;
  return QL_STATUS_OK;
}

static ql_status writer_bytes(ir_writer *writer, const void *data, size_t size,
                              ql_error *error) {
  ql_status status = writer_reserve(writer, size, error);
  if (status != QL_STATUS_OK) {
    return status;
  }
  if (size != 0u) {
    memcpy(writer->data + writer->size, data, size);
    writer->size += size;
  }
  return QL_STATUS_OK;
}

static ql_status writer_u32(ir_writer *writer, uint32_t value,
                            ql_error *error) {
  uint8_t bytes[4];
  bytes[0] = (uint8_t)(value & UINT32_C(0xff));
  bytes[1] = (uint8_t)((value >> 8u) & UINT32_C(0xff));
  bytes[2] = (uint8_t)((value >> 16u) & UINT32_C(0xff));
  bytes[3] = (uint8_t)((value >> 24u) & UINT32_C(0xff));
  return writer_bytes(writer, bytes, sizeof(bytes), error);
}

static ql_status writer_u64(ir_writer *writer, uint64_t value,
                            ql_error *error) {
  uint8_t bytes[8];
  size_t index;
  for (index = 0u; index < sizeof(bytes); ++index) {
    bytes[index] =
        (uint8_t)((value >> (uint32_t)(index * 8u)) & UINT64_C(0xff));
  }
  return writer_bytes(writer, bytes, sizeof(bytes), error);
}

static ql_status writer_size(ir_writer *writer, size_t value, ql_error *error) {
#if SIZE_MAX > UINT64_MAX
  if (value > (size_t)UINT64_MAX) {
    ql_error_set(error, QL_STATUS_INVALID_ARGUMENT,
                 "serialized IR field exceeds uint64_t");
    return QL_STATUS_INVALID_ARGUMENT;
  }
#endif
  return writer_u64(writer, (uint64_t)value, error);
}

static ql_status serialize_graph(const ir_graph *graph, uint8_t **data,
                                 size_t *size, ql_error *error) {
  ir_writer writer;
  size_t index;
  size_t nested;
  ql_status status;

  memset(&writer, 0, sizeof(writer));
  writer.allocator = &graph->allocator;
#define WRITE(expression_)                                                     \
  do {                                                                         \
    status = (expression_);                                                    \
    if (status != QL_STATUS_OK) {                                              \
      goto failure;                                                            \
    }                                                                          \
  } while (0)
  WRITE(writer_bytes(&writer, ir_magic, sizeof(ir_magic), error));
  WRITE(writer_u32(&writer, QL_IR_ARTIFACT_SCHEMA_VERSION, error));
  WRITE(writer_u32(&writer, (uint32_t)graph->cfg_kind, error));
  WRITE(writer_u32(&writer, (uint32_t)graph->type_count, error));
  WRITE(writer_u32(&writer, (uint32_t)graph->value_count, error));
  WRITE(writer_u32(&writer, (uint32_t)graph->block_count, error));
  WRITE(writer_u32(&writer, (uint32_t)graph->instruction_count, error));
  WRITE(writer_u32(&writer, graph->return_type, error));
  WRITE(writer_u32(&writer, graph->entry_block, error));
  WRITE(writer_size(&writer, graph->function_name_size, error));
  WRITE(writer_bytes(&writer, graph->function_name, graph->function_name_size,
                     error));

  for (index = 0u; index < graph->type_count; ++index) {
    const ir_type_entry *type = &graph->types[index];
    WRITE(writer_u32(&writer, (uint32_t)type->kind, error));
    WRITE(writer_u32(&writer, (uint32_t)type->float_format, error));
    WRITE(writer_u32(&writer, type->bit_width, error));
    WRITE(writer_u32(&writer, type->address_space, error));
    WRITE(writer_u32(&writer, type->element_type, error));
    WRITE(writer_u64(&writer, type->element_count, error));
  }
  for (index = 0u; index < graph->value_count; ++index) {
    const ir_value_entry *value = &graph->values[index];
    WRITE(writer_u32(&writer, value->type, error));
    WRITE(writer_u32(&writer, (uint32_t)value->definition_kind, error));
    WRITE(writer_u32(&writer, value->result_index, error));
    WRITE(writer_u32(&writer, value->instruction, error));
    WRITE(writer_size(&writer, value->constant_size, error));
    WRITE(writer_size(&writer, value->name_size, error));
    WRITE(writer_bytes(&writer, value->constant_data, value->constant_size,
                       error));
    WRITE(writer_bytes(&writer, value->name, value->name_size, error));
  }
  for (index = 0u; index < graph->instruction_count; ++index) {
    const ir_instruction_entry *instruction = &graph->instructions[index];
    WRITE(writer_u32(&writer, instruction->block, error));
    WRITE(writer_u32(&writer, instruction->opcode, error));
    WRITE(writer_u32(&writer, instruction->flags, error));
    WRITE(writer_u64(&writer, instruction->effects, error));
    WRITE(writer_u32(&writer, (uint32_t)instruction->operand_count, error));
    WRITE(
        writer_u32(&writer, (uint32_t)instruction->block_operand_count, error));
    WRITE(writer_u32(&writer, (uint32_t)instruction->result_count, error));
    WRITE(writer_u64(&writer, instruction->immediate, error));
    WRITE(writer_size(&writer, instruction->symbol_size, error));
    WRITE(writer_size(&writer, instruction->image_size, error));
    for (nested = 0u; nested < instruction->operand_count; ++nested) {
      WRITE(writer_u32(&writer, instruction->operands[nested], error));
    }
    for (nested = 0u; nested < instruction->block_operand_count; ++nested) {
      WRITE(writer_u32(&writer, instruction->block_operands[nested], error));
    }
    for (nested = 0u; nested < instruction->result_count; ++nested) {
      WRITE(writer_u32(&writer, instruction->results[nested], error));
    }
    WRITE(writer_bytes(&writer, instruction->symbol, instruction->symbol_size,
                       error));
    WRITE(writer_bytes(&writer, instruction->image, instruction->image_size,
                       error));
  }
  for (index = 0u; index < graph->block_count; ++index) {
    const ir_block_entry *block = &graph->blocks[index];
    const ir_terminator_entry *terminator = &block->terminator;
    WRITE(writer_size(&writer, block->label_size, error));
    WRITE(writer_u32(&writer, (uint32_t)block->instruction_count, error));
    WRITE(writer_u32(&writer, (uint32_t)terminator->kind, error));
    WRITE(writer_u32(&writer, terminator->condition, error));
    WRITE(writer_u32(&writer, terminator->return_value, error));
    WRITE(writer_u32(&writer, terminator->memory, error));
    WRITE(writer_u32(&writer, terminator->event_trace, error));
    WRITE(writer_u32(&writer, terminator->target, error));
    WRITE(writer_u32(&writer, terminator->false_target, error));
    WRITE(writer_u64(&writer, terminator->code, error));
    WRITE(writer_size(&writer, terminator->reason_size, error));
    for (nested = 0u; nested < block->instruction_count; ++nested) {
      WRITE(writer_u32(&writer, block->instructions[nested], error));
    }
    WRITE(writer_bytes(&writer, block->label, block->label_size, error));
    WRITE(writer_bytes(&writer, terminator->reason, terminator->reason_size,
                       error));
  }
#undef WRITE
  *data = writer.data;
  *size = writer.size;
  return QL_STATUS_OK;

failure:
#undef WRITE
  graph->allocator.deallocate(graph->allocator.user_data, writer.data);
  *data = NULL;
  *size = 0u;
  return status;
}

ql_status QL_CALL ql_ir_builder_finish(const ql_ir_builder *builder,
                                       ql_artifact **output, ql_error *error) {
  uint8_t *data = NULL;
  size_t size = 0u;
  ql_status status;

  if (builder == NULL || output == NULL) {
    ql_error_set(error, QL_STATUS_INVALID_ARGUMENT,
                 "IR builder and artifact output are required");
    return QL_STATUS_INVALID_ARGUMENT;
  }
  *output = NULL;
  status = graph_validate(&builder->graph, error);
  if (status != QL_STATUS_OK) {
    return status;
  }
  status = serialize_graph(&builder->graph, &data, &size, error);
  if (status == QL_STATUS_OK) {
    status = ql_artifact_create(&builder->graph.allocator, QL_ARTIFACT_KIND_IR,
                                QL_IR_ARTIFACT_SCHEMA_VERSION, data, size,
                                output, error);
  }
  builder->graph.allocator.deallocate(builder->graph.allocator.user_data, data);
  return status;
}

typedef struct ir_reader {
  const uint8_t *data;
  size_t size;
  size_t position;
} ir_reader;

static ql_status reader_take(ir_reader *reader, size_t size,
                             const uint8_t **output, ql_error *error) {
  size_t end;
  if (!checked_add_size(reader->position, size, &end) || end > reader->size) {
    ql_error_set(error, QL_STATUS_PARSE_ERROR,
                 "truncated quodlibet.ir artifact at byte %zu",
                 reader->position);
    return QL_STATUS_PARSE_ERROR;
  }
  *output = reader->data + reader->position;
  reader->position = end;
  return QL_STATUS_OK;
}

static ql_status reader_u32(ir_reader *reader, uint32_t *output,
                            ql_error *error) {
  const uint8_t *bytes;
  ql_status status = reader_take(reader, 4u, &bytes, error);
  if (status != QL_STATUS_OK) {
    return status;
  }
  *output = (uint32_t)bytes[0] | ((uint32_t)bytes[1] << 8u) |
            ((uint32_t)bytes[2] << 16u) | ((uint32_t)bytes[3] << 24u);
  return QL_STATUS_OK;
}

static ql_status reader_u64(ir_reader *reader, uint64_t *output,
                            ql_error *error) {
  const uint8_t *bytes;
  size_t index;
  uint64_t value = 0u;
  ql_status status = reader_take(reader, 8u, &bytes, error);
  if (status != QL_STATUS_OK) {
    return status;
  }
  for (index = 0u; index < 8u; ++index) {
    value |= (uint64_t)bytes[index] << (uint32_t)(index * 8u);
  }
  *output = value;
  return QL_STATUS_OK;
}

static ql_status reader_size(ir_reader *reader, size_t *output,
                             ql_error *error) {
  uint64_t value;
  ql_status status = reader_u64(reader, &value, error);
  if (status != QL_STATUS_OK) {
    return status;
  }
#if SIZE_MAX < UINT64_MAX
  if (value > (uint64_t)SIZE_MAX) {
    ql_error_set(error, QL_STATUS_PARSE_ERROR,
                 "quodlibet.ir size does not fit size_t");
    return QL_STATUS_PARSE_ERROR;
  }
#endif
  *output = (size_t)value;
  return QL_STATUS_OK;
}

static ql_status reader_owned_bytes(ir_reader *reader,
                                    const ql_allocator *allocator, size_t size,
                                    void **output, ql_error *error) {
  const uint8_t *bytes;
  ql_status status = reader_take(reader, size, &bytes, error);
  if (status != QL_STATUS_OK) {
    return status;
  }
  return copy_bytes(allocator, bytes, size, output, error);
}

static ql_status reader_owned_text(ir_reader *reader,
                                   const ql_allocator *allocator, size_t size,
                                   char **output, ql_error *error) {
  const uint8_t *bytes;
  ql_status status = reader_take(reader, size, &bytes, error);
  if (status != QL_STATUS_OK) {
    return status;
  }
  return copy_text(allocator, (const char *)bytes, size, output, error);
}

static ql_status reader_id_array(ir_reader *reader,
                                 const ql_allocator *allocator, size_t count,
                                 uint32_t **output, ql_error *error) {
  size_t byte_size;
  size_t index;
  ql_status status;

  *output = NULL;
  if (!checked_mul_size(count, sizeof(**output), &byte_size)) {
    ql_error_set(error, QL_STATUS_PARSE_ERROR,
                 "quodlibet.ir ID table size overflows size_t");
    return QL_STATUS_PARSE_ERROR;
  }
  if (byte_size > reader->size - reader->position) {
    ql_error_set(error, QL_STATUS_PARSE_ERROR,
                 "truncated quodlibet.ir ID table at byte %zu",
                 reader->position);
    return QL_STATUS_PARSE_ERROR;
  }
  if (byte_size != 0u) {
    *output = allocator->allocate(allocator->user_data, byte_size);
    if (*output == NULL) {
      ql_error_set(error, QL_STATUS_OUT_OF_MEMORY, NULL);
      return QL_STATUS_OUT_OF_MEMORY;
    }
  }
  for (index = 0u; index < count; ++index) {
    status = reader_u32(reader, &(*output)[index], error);
    if (status != QL_STATUS_OK) {
      allocator->deallocate(allocator->user_data, *output);
      *output = NULL;
      return status;
    }
  }
  return QL_STATUS_OK;
}

static ql_status reader_require_minimum_records(const ir_reader *reader,
                                                uint32_t count,
                                                size_t minimum_size,
                                                const char *table,
                                                ql_error *error) {
  size_t remaining = reader->size - reader->position;
  if ((size_t)count > remaining / minimum_size) {
    ql_error_set(error, QL_STATUS_PARSE_ERROR,
                 "quodlibet.ir %s count exceeds remaining bytes", table);
    return QL_STATUS_PARSE_ERROR;
  }
  return QL_STATUS_OK;
}

static ql_status parse_graph(const ql_allocator *allocator, const void *data,
                             size_t size, ir_graph *graph, ql_error *error) {
  ir_reader reader;
  const uint8_t *magic;
  uint32_t schema_version;
  uint32_t cfg_kind;
  uint32_t type_count;
  uint32_t value_count;
  uint32_t block_count;
  uint32_t instruction_count;
  size_t function_name_size;
  size_t index;
  size_t nested;
  ql_status status;

  graph_init(graph, allocator);
  reader.data = (const uint8_t *)data;
  reader.size = size;
  reader.position = 0u;
#define READ(expression_)                                                      \
  do {                                                                         \
    status = (expression_);                                                    \
    if (status != QL_STATUS_OK) {                                              \
      goto failure;                                                            \
    }                                                                          \
  } while (0)
  READ(reader_take(&reader, sizeof(ir_magic), &magic, error));
  if (memcmp(magic, ir_magic, sizeof(ir_magic)) != 0) {
    ql_error_set(error, QL_STATUS_PARSE_ERROR, "invalid quodlibet.ir magic");
    status = QL_STATUS_PARSE_ERROR;
    goto failure;
  }
  READ(reader_u32(&reader, &schema_version, error));
  if (schema_version != QL_IR_ARTIFACT_SCHEMA_VERSION) {
    ql_error_set(error, QL_STATUS_SCHEMA_MISMATCH,
                 "unsupported IR payload schema version %u", schema_version);
    status = QL_STATUS_SCHEMA_MISMATCH;
    goto failure;
  }
  READ(reader_u32(&reader, &cfg_kind, error));
  if (cfg_kind != (uint32_t)QL_IR_CFG_ACYCLIC &&
      cfg_kind != (uint32_t)QL_IR_CFG_CYCLIC) {
    ql_error_set(error, QL_STATUS_SCHEMA_MISMATCH, "unsupported IR CFG kind %u",
                 cfg_kind);
    status = QL_STATUS_SCHEMA_MISMATCH;
    goto failure;
  }
  graph->cfg_kind = (ql_ir_cfg_kind)cfg_kind;
  READ(reader_u32(&reader, &type_count, error));
  READ(reader_u32(&reader, &value_count, error));
  READ(reader_u32(&reader, &block_count, error));
  READ(reader_u32(&reader, &instruction_count, error));
  READ(reader_u32(&reader, &graph->return_type, error));
  READ(reader_u32(&reader, &graph->entry_block, error));
  READ(reader_size(&reader, &function_name_size, error));
  READ(reader_owned_text(&reader, allocator, function_name_size,
                         &graph->function_name, error));
  graph->function_name_size = function_name_size;
  graph->function_is_set = 1u;
  graph->parameters_are_sealed = 1u;

  READ(reader_require_minimum_records(&reader, type_count, 28u, "type", error));
  if (type_count != 0u) {
    READ(reserve_array(allocator, (void **)&graph->types, &graph->type_capacity,
                       (size_t)type_count, sizeof(*graph->types), error));
  }
  for (index = 0u; index < (size_t)type_count; ++index) {
    ir_type_entry *type = &graph->types[index];
    uint32_t kind;
    uint32_t format;
    graph->type_count = index + 1u;
    READ(reader_u32(&reader, &kind, error));
    READ(reader_u32(&reader, &format, error));
    type->kind = (ql_ir_type_kind)kind;
    type->float_format = (ql_ir_float_format)format;
    READ(reader_u32(&reader, &type->bit_width, error));
    READ(reader_u32(&reader, &type->address_space, error));
    READ(reader_u32(&reader, &type->element_type, error));
    READ(reader_u64(&reader, &type->element_count, error));
  }
  READ(reader_require_minimum_records(&reader, value_count, 32u, "value",
                                      error));
  if (value_count != 0u) {
    READ(reserve_array(allocator, (void **)&graph->values,
                       &graph->value_capacity, (size_t)value_count,
                       sizeof(*graph->values), error));
    memset(graph->values, 0, (size_t)value_count * sizeof(*graph->values));
  }
  for (index = 0u; index < (size_t)value_count; ++index) {
    ir_value_entry *value = &graph->values[index];
    uint32_t definition_kind;
    size_t constant_size;
    size_t name_size;
    graph->value_count = index + 1u;
    READ(reader_u32(&reader, &value->type, error));
    READ(reader_u32(&reader, &definition_kind, error));
    value->definition_kind = (ql_ir_value_definition_kind)definition_kind;
    READ(reader_u32(&reader, &value->result_index, error));
    READ(reader_u32(&reader, &value->instruction, error));
    READ(reader_size(&reader, &constant_size, error));
    READ(reader_size(&reader, &name_size, error));
    READ(reader_owned_bytes(&reader, allocator, constant_size,
                            (void **)&value->constant_data, error));
    value->constant_size = constant_size;
    READ(reader_owned_text(&reader, allocator, name_size, &value->name, error));
    value->name_size = name_size;
  }
  READ(reader_require_minimum_records(&reader, instruction_count, 56u,
                                      "instruction", error));
  if (instruction_count != 0u) {
    READ(reserve_array(allocator, (void **)&graph->instructions,
                       &graph->instruction_capacity, (size_t)instruction_count,
                       sizeof(*graph->instructions), error));
    memset(graph->instructions, 0,
           (size_t)instruction_count * sizeof(*graph->instructions));
  }
  for (index = 0u; index < (size_t)instruction_count; ++index) {
    ir_instruction_entry *instruction = &graph->instructions[index];
    uint32_t operand_count;
    uint32_t block_operand_count;
    uint32_t result_count;
    size_t symbol_size;
    size_t image_size;
    graph->instruction_count = index + 1u;
    READ(reader_u32(&reader, &instruction->block, error));
    READ(reader_u32(&reader, &instruction->opcode, error));
    READ(reader_u32(&reader, &instruction->flags, error));
    READ(reader_u64(&reader, &instruction->effects, error));
    READ(reader_u32(&reader, &operand_count, error));
    READ(reader_u32(&reader, &block_operand_count, error));
    READ(reader_u32(&reader, &result_count, error));
    instruction->operand_count = (size_t)operand_count;
    instruction->block_operand_count = (size_t)block_operand_count;
    instruction->result_count = (size_t)result_count;
    READ(reader_u64(&reader, &instruction->immediate, error));
    READ(reader_size(&reader, &symbol_size, error));
    READ(reader_size(&reader, &image_size, error));
    READ(reader_id_array(&reader, allocator, instruction->operand_count,
                         (uint32_t **)&instruction->operands, error));
    READ(reader_id_array(&reader, allocator, instruction->block_operand_count,
                         (uint32_t **)&instruction->block_operands, error));
    READ(reader_id_array(&reader, allocator, instruction->result_count,
                         (uint32_t **)&instruction->results, error));
    READ(reader_owned_text(&reader, allocator, symbol_size,
                           &instruction->symbol, error));
    instruction->symbol_size = symbol_size;
    READ(reader_owned_bytes(&reader, allocator, image_size, &instruction->image,
                            error));
    instruction->image_size = image_size;
  }
  READ(reader_require_minimum_records(&reader, block_count, 56u, "block",
                                      error));
  if (block_count != 0u) {
    READ(reserve_array(allocator, (void **)&graph->blocks,
                       &graph->block_capacity, (size_t)block_count,
                       sizeof(*graph->blocks), error));
    memset(graph->blocks, 0, (size_t)block_count * sizeof(*graph->blocks));
  }
  for (index = 0u; index < (size_t)block_count; ++index) {
    ir_block_entry *block = &graph->blocks[index];
    ir_terminator_entry *terminator = &block->terminator;
    size_t label_size;
    size_t reason_size;
    uint32_t block_instruction_count;
    uint32_t terminator_kind;
    graph->block_count = index + 1u;
    READ(reader_size(&reader, &label_size, error));
    READ(reader_u32(&reader, &block_instruction_count, error));
    READ(reader_u32(&reader, &terminator_kind, error));
    terminator->present = 1u;
    terminator->kind = (ql_ir_terminator_kind)terminator_kind;
    READ(reader_u32(&reader, &terminator->condition, error));
    READ(reader_u32(&reader, &terminator->return_value, error));
    READ(reader_u32(&reader, &terminator->memory, error));
    READ(reader_u32(&reader, &terminator->event_trace, error));
    READ(reader_u32(&reader, &terminator->target, error));
    READ(reader_u32(&reader, &terminator->false_target, error));
    READ(reader_u64(&reader, &terminator->code, error));
    READ(reader_size(&reader, &reason_size, error));
    block->instruction_count = (size_t)block_instruction_count;
    block->instruction_capacity = block->instruction_count;
    READ(reader_id_array(&reader, allocator, block->instruction_count,
                         (uint32_t **)&block->instructions, error));
    READ(reader_owned_text(&reader, allocator, label_size, &block->label,
                           error));
    block->label_size = label_size;
    READ(reader_owned_text(&reader, allocator, reason_size, &terminator->reason,
                           error));
    terminator->reason_size = reason_size;
  }
  (void)nested;
  if (reader.position != reader.size) {
    ql_error_set(error, QL_STATUS_PARSE_ERROR,
                 "quodlibet.ir artifact has %zu trailing bytes",
                 reader.size - reader.position);
    status = QL_STATUS_PARSE_ERROR;
    goto failure;
  }
#undef READ
  status = graph_validate(graph, error);
  if (status != QL_STATUS_OK) {
    goto failure_without_macro;
  }
  return QL_STATUS_OK;

failure:
#undef READ
failure_without_macro:
  graph_dispose(graph);
  return status;
}

ql_status QL_CALL ql_ir_open(const ql_allocator *allocator,
                             const ql_artifact *artifact, ql_ir **output,
                             ql_error *error) {
  const ql_allocator *selected = allocator;
  ql_artifact_view artifact_view;
  ir_graph graph;
  ql_ir *ir;
  ql_status status;

  if (artifact == NULL || output == NULL) {
    ql_error_set(error, QL_STATUS_INVALID_ARGUMENT,
                 "IR artifact and output are required");
    return QL_STATUS_INVALID_ARGUMENT;
  }
  *output = NULL;
  memset(&artifact_view, 0, sizeof(artifact_view));
  artifact_view.struct_size = sizeof(artifact_view);
  status = ql_artifact_get_view(artifact, &artifact_view, error);
  if (status != QL_STATUS_OK) {
    return status;
  }
  if (strcmp(artifact_view.kind, QL_ARTIFACT_KIND_IR) != 0) {
    ql_error_set(error, QL_STATUS_TYPE_MISMATCH,
                 "artifact kind '%s' is not quodlibet.ir", artifact_view.kind);
    return QL_STATUS_TYPE_MISMATCH;
  }
  if (artifact_view.schema_version != QL_IR_ARTIFACT_SCHEMA_VERSION) {
    ql_error_set(error, QL_STATUS_SCHEMA_MISMATCH,
                 "unsupported IR artifact schema version %u",
                 artifact_view.schema_version);
    return QL_STATUS_SCHEMA_MISMATCH;
  }
  if (selected == NULL) {
    selected = ql_default_allocator();
  }
  if (!ql_allocator_is_valid(selected)) {
    ql_error_set(error, QL_STATUS_INVALID_ARGUMENT, "invalid allocator");
    return QL_STATUS_INVALID_ARGUMENT;
  }
  status = parse_graph(selected, artifact_view.data, artifact_view.size, &graph,
                       error);
  if (status != QL_STATUS_OK) {
    return status;
  }
  ir = selected->allocate(selected->user_data, sizeof(*ir));
  if (ir == NULL) {
    graph_dispose(&graph);
    ql_error_set(error, QL_STATUS_OUT_OF_MEMORY, NULL);
    return QL_STATUS_OUT_OF_MEMORY;
  }
  memset(ir, 0, sizeof(*ir));
  ir->graph = graph;
  ir->artifact = (ql_artifact *)(uintptr_t)artifact;
  ir->artifact_digest = artifact_view.digest;
  ql_artifact_retain(ir->artifact);
  atomic_init(&ir->reference_count, 1u);
  *output = ir;
  ql_error_clear(error);
  return QL_STATUS_OK;
}

void QL_CALL ql_ir_retain(ql_ir *ir) {
  if (ir != NULL) {
    (void)atomic_fetch_add_explicit(&ir->reference_count, 1u,
                                    memory_order_relaxed);
  }
}

void QL_CALL ql_ir_release(ql_ir *ir) {
  ql_allocator allocator;

  if (ir == NULL || atomic_fetch_sub_explicit(&ir->reference_count, 1u,
                                              memory_order_acq_rel) != 1u) {
    return;
  }
  allocator = ir->graph.allocator;
  ql_artifact_release(ir->artifact);
  graph_dispose(&ir->graph);
  allocator.deallocate(allocator.user_data, ir);
}

static ql_status validate_view_output(const void *owner, const void *view,
                                      size_t struct_size, size_t required,
                                      const char *name, ql_error *error) {
  if (owner == NULL || view == NULL) {
    ql_error_set(error, QL_STATUS_INVALID_ARGUMENT,
                 "IR and %s view are required", name);
    return QL_STATUS_INVALID_ARGUMENT;
  }
  if (struct_size != 0u && struct_size < required) {
    ql_error_set(error, QL_STATUS_ABI_MISMATCH,
                 "IR %s view structure is too small", name);
    return QL_STATUS_ABI_MISMATCH;
  }
  return QL_STATUS_OK;
}

ql_status QL_CALL ql_ir_get_view(const ql_ir *ir, ql_ir_view_v1 *view,
                                 ql_error *error) {
  ql_status status =
      validate_view_output(ir, view, view != NULL ? view->struct_size : 0u,
                           sizeof(*view), "module", error);
  if (status != QL_STATUS_OK) {
    return status;
  }
  memset(view, 0, sizeof(*view));
  view->struct_size = sizeof(*view);
  view->schema_version = QL_IR_ARTIFACT_SCHEMA_VERSION;
  view->cfg_kind = ir->graph.cfg_kind;
  view->function_name = ir->graph.function_name;
  view->function_name_size = ir->graph.function_name_size;
  view->return_type = ir->graph.return_type;
  view->entry_block = ir->graph.entry_block;
  view->type_count = ir->graph.type_count;
  view->value_count = ir->graph.value_count;
  view->block_count = ir->graph.block_count;
  view->instruction_count = ir->graph.instruction_count;
  view->artifact_digest = ir->artifact_digest;
  ql_error_clear(error);
  return QL_STATUS_OK;
}

ql_status QL_CALL ql_ir_type_at(const ql_ir *ir, size_t index,
                                ql_ir_type_view_v1 *view, ql_error *error) {
  const ir_type_entry *type;
  ql_status status =
      validate_view_output(ir, view, view != NULL ? view->struct_size : 0u,
                           sizeof(*view), "type", error);
  if (status != QL_STATUS_OK) {
    return status;
  }
  if (index >= ir->graph.type_count) {
    ql_error_set(error, QL_STATUS_NOT_FOUND,
                 "IR type index %zu is out of range", index);
    return QL_STATUS_NOT_FOUND;
  }
  type = &ir->graph.types[index];
  memset(view, 0, sizeof(*view));
  view->struct_size = sizeof(*view);
  view->id = (ql_ir_type_id)index;
  view->kind = type->kind;
  view->float_format = type->float_format;
  view->bit_width = type->bit_width;
  view->address_space = type->address_space;
  view->element_type = type->element_type;
  view->element_count = type->element_count;
  ql_error_clear(error);
  return QL_STATUS_OK;
}

ql_status QL_CALL ql_ir_value_at(const ql_ir *ir, size_t index,
                                 ql_ir_value_view_v1 *view, ql_error *error) {
  const ir_value_entry *value;
  ql_status status =
      validate_view_output(ir, view, view != NULL ? view->struct_size : 0u,
                           sizeof(*view), "value", error);
  if (status != QL_STATUS_OK) {
    return status;
  }
  if (index >= ir->graph.value_count) {
    ql_error_set(error, QL_STATUS_NOT_FOUND,
                 "IR value index %zu is out of range", index);
    return QL_STATUS_NOT_FOUND;
  }
  value = &ir->graph.values[index];
  memset(view, 0, sizeof(*view));
  view->struct_size = sizeof(*view);
  view->id = (ql_ir_value_id)index;
  view->type = value->type;
  view->definition_kind = value->definition_kind;
  view->result_index = value->result_index;
  view->instruction = value->instruction;
  view->constant_data = value->constant_data;
  view->constant_size = value->constant_size;
  view->name = value->name;
  view->name_size = value->name_size;
  ql_error_clear(error);
  return QL_STATUS_OK;
}

ql_status QL_CALL ql_ir_instruction_at(const ql_ir *ir, size_t index,
                                       ql_ir_instruction_view_v1 *view,
                                       ql_error *error) {
  const ir_instruction_entry *instruction;
  ql_status status =
      validate_view_output(ir, view, view != NULL ? view->struct_size : 0u,
                           sizeof(*view), "instruction", error);
  if (status != QL_STATUS_OK) {
    return status;
  }
  if (index >= ir->graph.instruction_count) {
    ql_error_set(error, QL_STATUS_NOT_FOUND,
                 "IR instruction index %zu is out of range", index);
    return QL_STATUS_NOT_FOUND;
  }
  instruction = &ir->graph.instructions[index];
  memset(view, 0, sizeof(*view));
  view->struct_size = sizeof(*view);
  view->id = (ql_ir_instruction_id)index;
  view->block = instruction->block;
  view->opcode = instruction->opcode;
  view->flags = instruction->flags;
  view->effects = instruction->effects;
  view->operands = instruction->operands;
  view->operand_count = instruction->operand_count;
  view->block_operands = instruction->block_operands;
  view->block_operand_count = instruction->block_operand_count;
  view->results = instruction->results;
  view->result_count = instruction->result_count;
  view->immediate = instruction->immediate;
  view->symbol = instruction->symbol;
  view->symbol_size = instruction->symbol_size;
  view->image = instruction->image;
  view->image_size = instruction->image_size;
  ql_error_clear(error);
  return QL_STATUS_OK;
}

ql_status QL_CALL ql_ir_block_at(const ql_ir *ir, size_t index,
                                 ql_ir_block_view_v1 *view, ql_error *error) {
  const ir_block_entry *block;
  const ir_terminator_entry *terminator;
  ql_status status =
      validate_view_output(ir, view, view != NULL ? view->struct_size : 0u,
                           sizeof(*view), "block", error);
  if (status != QL_STATUS_OK) {
    return status;
  }
  if (index >= ir->graph.block_count) {
    ql_error_set(error, QL_STATUS_NOT_FOUND,
                 "IR block index %zu is out of range", index);
    return QL_STATUS_NOT_FOUND;
  }
  block = &ir->graph.blocks[index];
  terminator = &block->terminator;
  memset(view, 0, sizeof(*view));
  view->struct_size = sizeof(*view);
  view->id = (ql_ir_block_id)index;
  view->label = block->label;
  view->label_size = block->label_size;
  view->instructions = block->instructions;
  view->instruction_count = block->instruction_count;
  ql_ir_terminator_definition_init(&view->terminator, terminator->kind);
  view->terminator.condition = terminator->condition;
  view->terminator.return_value = terminator->return_value;
  view->terminator.memory = terminator->memory;
  view->terminator.event_trace = terminator->event_trace;
  view->terminator.target = terminator->target;
  view->terminator.false_target = terminator->false_target;
  view->terminator.code = terminator->code;
  view->terminator.reason = terminator->reason;
  view->terminator.reason_size = terminator->reason_size;
  ql_error_clear(error);
  return QL_STATUS_OK;
}

/* internal.h records the contract: every refutation path that replays a
   solver model concretely gates on this, because running a call needs a
   callee the witness does not carry. */
static int ir_side_threads_event_trace(const ql_ir *ir) {
  ql_ir_view_v1 view;
  ql_error ignored;
  size_t index;

  memset(&view, 0, sizeof(view));
  view.struct_size = sizeof(view);
  if (ql_ir_get_view(ir, &view, &ignored) != QL_STATUS_OK) {
    return 0;
  }
  for (index = 0u; index < view.value_count; ++index) {
    ql_ir_value_view_v1 value;
    ql_ir_type_view_v1 type;
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

int ql_internal_ir_threads_event_trace(const ql_ir *left, const ql_ir *right) {
  return ir_side_threads_event_trace(left) != 0 ||
         ir_side_threads_event_trace(right) != 0;
}
