#include "quodlibet/egraph_method.h"

#include <inttypes.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "quodlibet/egraph.h"
#include "quodlibet/egraph_check.h"
#include "quodlibet/ir.h"
#include "quodlibet/ir_verify.h"

#include "yyjson.h"

typedef struct egraph_method_instance {
  ql_allocator allocator;
  ql_egraph_config_v1 config;
  ql_egraph_saturation_limits_v1 limits;
} egraph_method_instance;

typedef struct egraph_method_state {
  egraph_method_instance *instance;
  const ql_ir *input;
  ql_ir_view_v1 input_view;
  ql_egraph *graph;
  ql_egraph_check_report *check_report;
  ql_ir_builder *builder;
  ql_egraph_term_id *value_terms;
  ql_egraph_term_id *chosen_terms;
  uint8_t *value_is_leaf;
  uint8_t *value_is_normalized;
  size_t *value_positions;
  ql_ir_type_id *type_map;
  ql_ir_value_id *value_map;
  ql_ir_value_id *term_values;
  ql_ir_value_id *term_sources;
  size_t *term_positions;
  uint8_t *needed_terms;
  ql_egraph_term_id *term_stack;
  size_t term_count;
  ql_egraph_term_id bool_false;
  ql_egraph_term_id bool_true;
  ql_ir_block_id output_block;
} egraph_method_state;

static void *egraph_json_allocate(void *context, size_t size) {
  ql_allocator *allocator = (ql_allocator *)context;
  return allocator->allocate(allocator->user_data, size);
}

static void *egraph_json_reallocate(void *context, void *pointer,
                                    size_t old_size, size_t size) {
  ql_allocator *allocator = (ql_allocator *)context;
  (void)old_size;
  return allocator->reallocate(allocator->user_data, pointer, size);
}

static void egraph_json_deallocate(void *context, void *pointer) {
  ql_allocator *allocator = (ql_allocator *)context;
  allocator->deallocate(allocator->user_data, pointer);
}

static yyjson_alc egraph_json_allocator(ql_allocator *allocator) {
  yyjson_alc result;
  result.malloc = egraph_json_allocate;
  result.realloc = egraph_json_reallocate;
  result.free = egraph_json_deallocate;
  result.ctx = allocator;
  return result;
}

static int json_uint(yyjson_val *root, const char *name, uint64_t *output,
                     ql_error *error) {
  yyjson_val *value = yyjson_obj_get(root, name);
  if (value == NULL) {
    return 1;
  }
  if (!yyjson_is_uint(value)) {
    ql_error_set(error, QL_STATUS_INVALID_ARGUMENT,
                 "%s must be a positive integer", name);
    return 0;
  }
  *output = yyjson_get_uint(value);
  if (*output == 0u) {
    ql_error_set(error, QL_STATUS_INVALID_ARGUMENT,
                 "%s must be a positive integer", name);
    return 0;
  }
  return 1;
}

static ql_status parse_options(egraph_method_instance *instance,
                               const char *options_json, ql_error *error) {
  static const char *const known[] = {
      "rewrite_set",   "node_limit",    "merge_limit",      "iteration_limit",
      "rewrite_limit", "max_bit_width", "reconstruct_proof"};
  yyjson_alc allocator = egraph_json_allocator(&instance->allocator);
  yyjson_read_err read_error;
  yyjson_doc *document;
  yyjson_val *root;
  yyjson_obj_iter iterator;
  yyjson_val *key;
  yyjson_val *value;
  uint64_t integer;
  ql_status status = QL_STATUS_OK;

  if (options_json == NULL || options_json[0] == '\0') {
    ql_error_clear(error);
    return QL_STATUS_OK;
  }
  document =
      yyjson_read_opts((char *)(uintptr_t)options_json, strlen(options_json),
                       0u, &allocator, &read_error);
  if (document == NULL) {
    ql_error_set(error, QL_STATUS_PARSE_ERROR,
                 "invalid %s options at byte %zu: %s", QL_EGRAPH_METHOD_NAME,
                 read_error.pos,
                 read_error.msg != NULL ? read_error.msg : "parse error");
    return QL_STATUS_PARSE_ERROR;
  }
  root = yyjson_doc_get_root(document);
  if (!yyjson_is_obj(root)) {
    ql_error_set(error, QL_STATUS_PARSE_ERROR,
                 "%s options must be a JSON object", QL_EGRAPH_METHOD_NAME);
    status = QL_STATUS_PARSE_ERROR;
    goto cleanup;
  }
  yyjson_obj_iter_init(root, &iterator);
  while ((key = yyjson_obj_iter_next(&iterator)) != NULL) {
    const char *name = yyjson_get_str(key);
    size_t index;
    int recognized = 0;
    for (index = 0u; index < sizeof(known) / sizeof(known[0]); ++index) {
      if (name != NULL && strcmp(name, known[index]) == 0) {
        recognized = 1;
        break;
      }
    }
    if (!recognized) {
      ql_error_set(error, QL_STATUS_INVALID_ARGUMENT,
                   "%s does not accept the option '%s'", QL_EGRAPH_METHOD_NAME,
                   name != NULL ? name : "");
      status = QL_STATUS_INVALID_ARGUMENT;
      goto cleanup;
    }
  }
  value = yyjson_obj_get(root, "rewrite_set");
  if (value != NULL) {
    const char *text = yyjson_get_str(value);
    if (text == NULL || strcmp(text, "pure-bitvector-v1") != 0) {
      ql_error_set(error, QL_STATUS_INVALID_ARGUMENT,
                   "rewrite_set must be \"pure-bitvector-v1\"");
      status = QL_STATUS_INVALID_ARGUMENT;
      goto cleanup;
    }
  }
  value = yyjson_obj_get(root, "reconstruct_proof");
  if (value != NULL && (!yyjson_is_bool(value) || !yyjson_get_bool(value))) {
    ql_error_set(error, QL_STATUS_INVALID_ARGUMENT,
                 "reconstruct_proof must be true; unchecked rewrites are "
                 "not an available mode");
    status = QL_STATUS_INVALID_ARGUMENT;
    goto cleanup;
  }
  integer = instance->config.max_terms;
  if (!json_uint(root, "node_limit", &integer, error) ||
      integer > (uint64_t)UINT32_MAX - 1u) {
    if (error->code == QL_STATUS_OK) {
      ql_error_set(error, QL_STATUS_INVALID_ARGUMENT,
                   "node_limit exceeds the e-graph ID space");
    }
    status = QL_STATUS_INVALID_ARGUMENT;
    goto cleanup;
  }
  instance->config.max_terms = integer;
  instance->config.max_classes = integer;
  integer = instance->config.max_merges;
  if (!json_uint(root, "merge_limit", &integer, error)) {
    status = QL_STATUS_INVALID_ARGUMENT;
    goto cleanup;
  }
  instance->config.max_merges = integer;
  integer = instance->limits.max_iterations;
  if (!json_uint(root, "iteration_limit", &integer, error) ||
      integer > UINT32_MAX) {
    if (error->code == QL_STATUS_OK) {
      ql_error_set(error, QL_STATUS_INVALID_ARGUMENT,
                   "iteration_limit exceeds uint32");
    }
    status = QL_STATUS_INVALID_ARGUMENT;
    goto cleanup;
  }
  instance->limits.max_iterations = (uint32_t)integer;
  integer = instance->limits.max_rewrite_applications;
  if (!json_uint(root, "rewrite_limit", &integer, error)) {
    status = QL_STATUS_INVALID_ARGUMENT;
    goto cleanup;
  }
  instance->limits.max_rewrite_applications = integer;
  integer = instance->config.max_bit_width;
  if (!json_uint(root, "max_bit_width", &integer, error) ||
      integer > UINT32_MAX) {
    if (error->code == QL_STATUS_OK) {
      ql_error_set(error, QL_STATUS_INVALID_ARGUMENT,
                   "max_bit_width exceeds uint32");
    }
    status = QL_STATUS_INVALID_ARGUMENT;
    goto cleanup;
  }
  instance->config.max_bit_width = (uint32_t)integer;

cleanup:
  yyjson_doc_free(document);
  if (status == QL_STATUS_OK) {
    ql_error_clear(error);
  }
  return status;
}

static ql_status QL_CALL egraph_method_create(const ql_host_v1 *host,
                                              const char *options_json,
                                              void **output, ql_error *error) {
  egraph_method_instance *instance;
  ql_status status;

  if (host == NULL || output == NULL ||
      !ql_allocator_is_valid(&host->allocator)) {
    ql_error_set(error, QL_STATUS_INVALID_ARGUMENT,
                 "%s requires a host allocator and instance output",
                 QL_EGRAPH_METHOD_NAME);
    return QL_STATUS_INVALID_ARGUMENT;
  }
  *output = NULL;
  instance =
      host->allocator.allocate(host->allocator.user_data, sizeof(*instance));
  if (instance == NULL) {
    ql_error_set(error, QL_STATUS_OUT_OF_MEMORY, NULL);
    return QL_STATUS_OUT_OF_MEMORY;
  }
  memset(instance, 0, sizeof(*instance));
  instance->allocator = host->allocator;
  ql_egraph_config_init(&instance->config);
  ql_egraph_saturation_limits_init(&instance->limits);
  status = parse_options(instance, options_json, error);
  if (status != QL_STATUS_OK) {
    instance->allocator.deallocate(instance->allocator.user_data, instance);
    return status;
  }
  *output = instance;
  ql_error_clear(error);
  return QL_STATUS_OK;
}

static void QL_CALL egraph_method_destroy(void *opaque) {
  egraph_method_instance *instance = (egraph_method_instance *)opaque;
  if (instance != NULL) {
    instance->allocator.deallocate(instance->allocator.user_data, instance);
  }
}

static ql_status open_supported_ir(ql_artifact *const *inputs,
                                   size_t input_count, ql_ir **output,
                                   ql_ir_view_v1 *view, ql_error *error) {
  ql_artifact_view artifact_view;
  ql_ir_verify_report_v1 report;
  ql_ir *ir = NULL;
  ql_status status;

  if (inputs == NULL || input_count != 1u || inputs[0] == NULL ||
      output == NULL || view == NULL) {
    ql_error_set(error, QL_STATUS_INVALID_ARGUMENT,
                 "%s requires exactly one IR artifact", QL_EGRAPH_METHOD_NAME);
    return QL_STATUS_INVALID_ARGUMENT;
  }
  *output = NULL;
  memset(&artifact_view, 0, sizeof(artifact_view));
  artifact_view.struct_size = sizeof(artifact_view);
  status = ql_artifact_get_view(inputs[0], &artifact_view, error);
  if (status != QL_STATUS_OK) {
    return status;
  }
  if (strcmp(artifact_view.kind, QL_ARTIFACT_KIND_IR) != 0) {
    ql_error_set(error, QL_STATUS_TYPE_MISMATCH,
                 "%s input kind is '%s', not '%s'", QL_EGRAPH_METHOD_NAME,
                 artifact_view.kind, QL_ARTIFACT_KIND_IR);
    return QL_STATUS_TYPE_MISMATCH;
  }
  status = ql_ir_open(NULL, inputs[0], &ir, error);
  if (status != QL_STATUS_OK) {
    return status;
  }
  memset(view, 0, sizeof(*view));
  view->struct_size = sizeof(*view);
  status = ql_ir_get_view(ir, view, error);
  if (status != QL_STATUS_OK) {
    ql_ir_release(ir);
    return status;
  }
  if (view->cfg_kind != QL_IR_CFG_ACYCLIC || view->block_count != 1u) {
    ql_ir_release(ir);
    ql_error_set(error, QL_STATUS_TYPE_MISMATCH,
                 "%s currently accepts one acyclic basic block",
                 QL_EGRAPH_METHOD_NAME);
    return QL_STATUS_TYPE_MISMATCH;
  }
  ql_ir_verify_report_init(&report);
  status = ql_ir_verify(NULL, ir, &report, error);
  if (status != QL_STATUS_OK) {
    ql_ir_release(ir);
    return status;
  }
  *output = ir;
  ql_error_clear(error);
  return QL_STATUS_OK;
}

static ql_status QL_CALL egraph_method_validate(void *opaque,
                                                ql_artifact *const *inputs,
                                                size_t input_count,
                                                ql_error *error) {
  ql_ir *ir = NULL;
  ql_ir_view_v1 view;
  ql_status status;
  (void)opaque;

  status = open_supported_ir(inputs, input_count, &ir, &view, error);
  ql_ir_release(ir);
  return status;
}

static int run_cancelled(const ql_run_context_v1 *context) {
  return context != NULL && context->is_cancelled != NULL &&
         context->is_cancelled(context->cancel_state) != 0u;
}

static void *allocate_items(egraph_method_state *state, size_t count,
                            size_t item_size, ql_error *error) {
  void *result;
  if (count != 0u && item_size > SIZE_MAX / count) {
    ql_error_set(error, QL_STATUS_OUT_OF_MEMORY,
                 "e-graph method allocation size overflowed");
    return NULL;
  }
  result = state->instance->allocator.allocate(
      state->instance->allocator.user_data, count * item_size);
  if (result == NULL && count != 0u) {
    ql_error_set(error, QL_STATUS_OUT_OF_MEMORY, NULL);
  }
  return result;
}

static ql_status value_type(const egraph_method_state *state,
                            ql_ir_value_id value, ql_ir_type_view_v1 *type,
                            ql_error *error) {
  ql_ir_value_view_v1 value_view;
  ql_status status;
  memset(&value_view, 0, sizeof(value_view));
  value_view.struct_size = sizeof(value_view);
  status = ql_ir_value_at(state->input, value, &value_view, error);
  if (status != QL_STATUS_OK) {
    return status;
  }
  memset(type, 0, sizeof(*type));
  type->struct_size = sizeof(*type);
  return ql_ir_type_at(state->input, value_view.type, type, error);
}

static int type_is_graphable(const egraph_method_state *state,
                             const ql_ir_type_view_v1 *type) {
  return type->kind == QL_IR_TYPE_BOOL ||
         (type->kind == QL_IR_TYPE_BIT_VECTOR && type->bit_width != 0u &&
          type->bit_width <= state->instance->config.max_bit_width);
}

static ql_status make_variable_term(egraph_method_state *state,
                                    ql_ir_value_id value,
                                    ql_egraph_term_id *output,
                                    ql_error *error) {
  ql_ir_type_view_v1 type;
  char symbol[32];
  int length;
  ql_status status = value_type(state, value, &type, error);
  if (status != QL_STATUS_OK) {
    return status;
  }
  if (!type_is_graphable(state, &type)) {
    *output = QL_EGRAPH_INVALID_TERM;
    return QL_STATUS_OK;
  }
  length = snprintf(symbol, sizeof(symbol), "v%" PRIu32, value);
  if (length < 0 || (size_t)length >= sizeof(symbol)) {
    ql_error_set(error, QL_STATUS_INTERNAL_ERROR,
                 "e-graph variable symbol overflowed");
    return QL_STATUS_INTERNAL_ERROR;
  }
  if (type.kind == QL_IR_TYPE_BOOL) {
    return ql_egraph_make_bool_variable(state->graph, symbol, output, error);
  }
  return ql_egraph_make_bv_variable(state->graph, symbol, type.bit_width,
                                    output, error);
}

static ql_status make_constant_term(egraph_method_state *state,
                                    const ql_ir_value_view_v1 *value,
                                    ql_egraph_term_id *output,
                                    ql_error *error) {
  ql_ir_type_view_v1 type;
  ql_status status;
  memset(&type, 0, sizeof(type));
  type.struct_size = sizeof(type);
  status = ql_ir_type_at(state->input, value->type, &type, error);
  if (status != QL_STATUS_OK) {
    return status;
  }
  if (!type_is_graphable(state, &type)) {
    *output = QL_EGRAPH_INVALID_TERM;
    return QL_STATUS_OK;
  }
  if (type.kind == QL_IR_TYPE_BOOL) {
    const uint8_t *byte = (const uint8_t *)value->constant_data;
    return ql_egraph_make_bool_constant(
        state->graph, byte != NULL ? byte[0] : 0u, output, error);
  }
  return ql_egraph_make_bv_constant(state->graph, type.bit_width,
                                    (const uint8_t *)value->constant_data,
                                    value->constant_size, output, error);
}

static ql_egraph_operator graph_operator(ql_ir_opcode opcode) {
  switch (opcode) {
  case QL_IR_OPCODE_BOOL_NOT:
    return QL_EGRAPH_OP_BOOL_NOT;
  case QL_IR_OPCODE_BV_NOT:
    return QL_EGRAPH_OP_BV_NOT;
  case QL_IR_OPCODE_ADD:
    return QL_EGRAPH_OP_BV_ADD;
  case QL_IR_OPCODE_SUB:
    return QL_EGRAPH_OP_BV_SUB;
  case QL_IR_OPCODE_MUL:
    return QL_EGRAPH_OP_BV_MUL;
  case QL_IR_OPCODE_BV_AND:
    return QL_EGRAPH_OP_BV_AND;
  case QL_IR_OPCODE_BV_OR:
    return QL_EGRAPH_OP_BV_OR;
  case QL_IR_OPCODE_BV_XOR:
    return QL_EGRAPH_OP_BV_XOR;
  case QL_IR_OPCODE_EQ:
    return QL_EGRAPH_OP_EQUAL;
  case QL_IR_OPCODE_SELECT:
    return QL_EGRAPH_OP_ITE;
  default:
    return QL_EGRAPH_OP_INVALID;
  }
}

static ql_status make_instruction_term(egraph_method_state *state,
                                       const ql_ir_instruction_view_v1 *view,
                                       ql_egraph_term_id *output,
                                       uint32_t *normalized, ql_error *error) {
  ql_egraph_term_id operands[QL_EGRAPH_MAX_ARITY];
  ql_egraph_operator op;
  size_t index;

  *normalized = 0u;
  *output = QL_EGRAPH_INVALID_TERM;
  if (view->effects != QL_IR_EFFECT_NONE || view->result_count != 1u ||
      view->block_operand_count != 0u ||
      view->operand_count > QL_EGRAPH_MAX_ARITY) {
    return QL_STATUS_OK;
  }
  for (index = 0u; index < view->operand_count; ++index) {
    operands[index] = state->value_terms[view->operands[index]];
    if (operands[index] == QL_EGRAPH_INVALID_TERM) {
      return QL_STATUS_OK;
    }
  }
  if (view->opcode == QL_IR_OPCODE_IDENTITY && view->operand_count == 1u) {
    *output = operands[0];
    *normalized = 1u;
    return QL_STATUS_OK;
  }
  if (view->opcode == QL_IR_OPCODE_BV_NEG && view->operand_count == 1u) {
    ql_ir_type_view_v1 type;
    ql_egraph_term_id zero;
    ql_status status = value_type(state, view->results[0], &type, error);
    if (status != QL_STATUS_OK) {
      return status;
    }
    status =
        ql_egraph_make_bv_u64(state->graph, type.bit_width, 0u, &zero, error);
    if (status != QL_STATUS_OK) {
      return status;
    }
    operands[1] = operands[0];
    operands[0] = zero;
    status = ql_egraph_make_operation(state->graph, QL_EGRAPH_OP_BV_SUB,
                                      operands, 2u, output, error);
    if (status == QL_STATUS_OK) {
      *normalized = 1u;
    }
    return status;
  }
  if (view->opcode == QL_IR_OPCODE_NE && view->operand_count == 2u) {
    ql_egraph_term_id equal;
    ql_status status = ql_egraph_make_operation(
        state->graph, QL_EGRAPH_OP_EQUAL, operands, 2u, &equal, error);
    if (status != QL_STATUS_OK) {
      return status;
    }
    status = ql_egraph_make_operation(state->graph, QL_EGRAPH_OP_BOOL_NOT,
                                      &equal, 1u, output, error);
    if (status == QL_STATUS_OK) {
      *normalized = 1u;
    }
    return status;
  }
  op = graph_operator(view->opcode);
  if (op == QL_EGRAPH_OP_INVALID) {
    return QL_STATUS_OK;
  }
  if (!ql_egraph_operator_is_supported(op)) {
    return QL_STATUS_OK;
  }
  {
    ql_status status = ql_egraph_make_operation(
        state->graph, op, operands, view->operand_count, output, error);
    if (status == QL_STATUS_OK) {
      *normalized = 1u;
    }
    return status;
  }
}

static ql_status initialize_value_positions(egraph_method_state *state,
                                            ql_error *error) {
  ql_ir_block_view_v1 block;
  size_t index;
  memset(state->value_positions, 0,
         state->input_view.value_count * sizeof(*state->value_positions));
  memset(&block, 0, sizeof(block));
  block.struct_size = sizeof(block);
  if (ql_ir_block_at(state->input, 0u, &block, error) != QL_STATUS_OK) {
    return error->code;
  }
  for (index = 0u; index < block.instruction_count; ++index) {
    ql_ir_instruction_view_v1 instruction;
    size_t result;
    memset(&instruction, 0, sizeof(instruction));
    instruction.struct_size = sizeof(instruction);
    if (ql_ir_instruction_at(state->input, block.instructions[index],
                             &instruction, error) != QL_STATUS_OK) {
      return error->code;
    }
    for (result = 0u; result < instruction.result_count; ++result) {
      state->value_positions[instruction.results[result]] = index + 1u;
    }
  }
  return QL_STATUS_OK;
}

static ql_status build_graph(egraph_method_state *state, ql_error *error) {
  ql_ir_block_view_v1 block;
  size_t index;
  ql_status status;

  status = ql_egraph_create(&state->instance->config,
                            &state->instance->allocator, &state->graph, error);
  if (status != QL_STATUS_OK) {
    return status;
  }
  for (index = 0u; index < state->input_view.value_count; ++index) {
    ql_ir_value_view_v1 value;
    memset(&value, 0, sizeof(value));
    value.struct_size = sizeof(value);
    status = ql_ir_value_at(state->input, index, &value, error);
    if (status != QL_STATUS_OK) {
      return status;
    }
    if (value.definition_kind == QL_IR_VALUE_PARAMETER) {
      status = make_variable_term(state, value.id,
                                  &state->value_terms[value.id], error);
      state->value_is_leaf[value.id] = 1u;
    } else if (value.definition_kind == QL_IR_VALUE_CONSTANT) {
      status = make_constant_term(state, &value, &state->value_terms[value.id],
                                  error);
      state->value_is_leaf[value.id] = 1u;
    }
    if (status != QL_STATUS_OK) {
      return status;
    }
  }
  memset(&block, 0, sizeof(block));
  block.struct_size = sizeof(block);
  status = ql_ir_block_at(state->input, 0u, &block, error);
  if (status != QL_STATUS_OK) {
    return status;
  }
  for (index = 0u; index < block.instruction_count; ++index) {
    ql_ir_instruction_view_v1 instruction;
    ql_egraph_term_id term = QL_EGRAPH_INVALID_TERM;
    uint32_t normalized = 0u;
    size_t result;
    memset(&instruction, 0, sizeof(instruction));
    instruction.struct_size = sizeof(instruction);
    status = ql_ir_instruction_at(state->input, block.instructions[index],
                                  &instruction, error);
    if (status != QL_STATUS_OK) {
      return status;
    }
    status =
        make_instruction_term(state, &instruction, &term, &normalized, error);
    if (status != QL_STATUS_OK) {
      return status;
    }
    if (normalized != 0u) {
      state->value_terms[instruction.results[0]] = term;
      state->value_is_normalized[instruction.results[0]] = 1u;
      continue;
    }
    for (result = 0u; result < instruction.result_count; ++result) {
      const ql_ir_value_id value = instruction.results[result];
      status =
          make_variable_term(state, value, &state->value_terms[value], error);
      if (status != QL_STATUS_OK) {
        return status;
      }
      state->value_is_leaf[value] = 1u;
    }
  }
  status =
      ql_egraph_make_bool_constant(state->graph, 0u, &state->bool_false, error);
  if (status == QL_STATUS_OK) {
    status = ql_egraph_make_bool_constant(state->graph, 1u, &state->bool_true,
                                          error);
  }
  return status;
}

static ql_status allocate_term_tables(egraph_method_state *state,
                                      ql_error *error) {
  size_t index;
  state->term_count = (size_t)ql_egraph_term_count(state->graph);
  if (state->term_count == SIZE_MAX) {
    ql_error_set(error, QL_STATUS_OUT_OF_MEMORY,
                 "e-graph term table is too large");
    return QL_STATUS_OUT_OF_MEMORY;
  }
  state->term_values = allocate_items(state, state->term_count + 1u,
                                      sizeof(*state->term_values), error);
  state->term_sources = allocate_items(state, state->term_count + 1u,
                                       sizeof(*state->term_sources), error);
  state->term_positions = allocate_items(state, state->term_count + 1u,
                                         sizeof(*state->term_positions), error);
  state->needed_terms = allocate_items(state, state->term_count + 1u,
                                       sizeof(*state->needed_terms), error);
  state->term_stack = allocate_items(state, state->term_count + 1u,
                                     sizeof(*state->term_stack), error);
  if (state->term_values == NULL || state->term_sources == NULL ||
      state->term_positions == NULL || state->needed_terms == NULL ||
      state->term_stack == NULL) {
    return QL_STATUS_OUT_OF_MEMORY;
  }
  memset(state->needed_terms, 0,
         (state->term_count + 1u) * sizeof(*state->needed_terms));
  for (index = 0u; index <= state->term_count; ++index) {
    state->term_values[index] = QL_IR_INVALID_VALUE_ID;
    state->term_sources[index] = QL_IR_INVALID_VALUE_ID;
    state->term_positions[index] = 0u;
  }
  for (index = 0u; index < state->input_view.value_count; ++index) {
    const ql_egraph_term_id term = state->value_terms[index];
    if (state->value_is_leaf[index] != 0u && term != QL_EGRAPH_INVALID_TERM &&
        state->term_sources[term] == QL_IR_INVALID_VALUE_ID) {
      state->term_sources[term] = (ql_ir_value_id)index;
    }
  }
  for (index = 1u; index <= state->term_count; ++index) {
    ql_egraph_term_view_v1 term;
    size_t operand;
    size_t position = 0u;
    memset(&term, 0, sizeof(term));
    if (ql_egraph_get_term(state->graph, (ql_egraph_term_id)index, &term,
                           error) != QL_STATUS_OK) {
      return error->code;
    }
    if (term.op == QL_EGRAPH_OP_VARIABLE) {
      const ql_ir_value_id source = state->term_sources[index];
      if (source == QL_IR_INVALID_VALUE_ID) {
        ql_error_set(error, QL_STATUS_INTERNAL_ERROR,
                     "e-graph variable term has no IR source");
        return QL_STATUS_INTERNAL_ERROR;
      }
      position = state->value_positions[source];
    } else {
      for (operand = 0u; operand < term.operand_count; ++operand) {
        const size_t incoming = state->term_positions[term.operands[operand]];
        if (incoming > position) {
          position = incoming;
        }
      }
    }
    state->term_positions[index] = position;
  }
  return QL_STATUS_OK;
}

static ql_status choose_terms(egraph_method_state *state, ql_error *error) {
  ql_egraph_check_report_view_v1 report_view;
  size_t index;
  ql_status status;

  memset(&report_view, 0, sizeof(report_view));
  report_view.struct_size = sizeof(report_view);
  status =
      ql_egraph_check_report_get_view(state->check_report, &report_view, error);
  if (status != QL_STATUS_OK) {
    return status;
  }
  if (report_view.all_merges_justified == 0u ||
      report_view.assumed_count != 0u || report_view.rejected_count != 0u) {
    ql_error_set(error, QL_STATUS_METHOD_ERROR,
                 "e-graph replay did not justify every merge "
                 "(assumed=%llu, rejected=%llu)",
                 (unsigned long long)report_view.assumed_count,
                 (unsigned long long)report_view.rejected_count);
    return QL_STATUS_METHOD_ERROR;
  }
  for (index = 0u; index < state->input_view.value_count; ++index) {
    const ql_egraph_term_id root = state->value_terms[index];
    ql_egraph_term_id extracted;
    uint64_t cost;
    uint32_t equal = 0u;
    if (state->value_is_normalized[index] == 0u) {
      continue;
    }
    status = ql_egraph_extract(state->graph, root, &extracted, &cost, error);
    (void)cost;
    if (status != QL_STATUS_OK) {
      return status;
    }
    status = ql_egraph_check_terms_equal(state->check_report, root, extracted,
                                         &equal, error);
    if (status != QL_STATUS_OK) {
      return status;
    }
    if (equal == 0u) {
      ql_error_set(error, QL_STATUS_METHOD_ERROR,
                   "independent replay did not establish extracted "
                   "term equality");
      return QL_STATUS_METHOD_ERROR;
    }
    if (state->term_positions[extracted] >= state->value_positions[index]) {
      extracted = root;
    }
    state->chosen_terms[index] = extracted;
    state->needed_terms[extracted] = 1u;
  }
  for (index = state->term_count; index != 0u; --index) {
    ql_egraph_term_view_v1 term;
    size_t operand;
    if (state->needed_terms[index] == 0u) {
      continue;
    }
    memset(&term, 0, sizeof(term));
    status = ql_egraph_get_term(state->graph, (ql_egraph_term_id)index, &term,
                                error);
    if (status != QL_STATUS_OK) {
      return status;
    }
    for (operand = 0u; operand < term.operand_count; ++operand) {
      state->needed_terms[term.operands[operand]] = 1u;
    }
    if (term.op == QL_EGRAPH_OP_BOOL_AND) {
      state->needed_terms[state->bool_false] = 1u;
    } else if (term.op == QL_EGRAPH_OP_BOOL_OR) {
      state->needed_terms[state->bool_true] = 1u;
    }
  }
  return QL_STATUS_OK;
}

static ql_status clone_types_and_function(egraph_method_state *state,
                                          ql_error *error) {
  size_t index;
  ql_status status;
  for (index = 0u; index < state->input_view.type_count; ++index) {
    ql_ir_type_view_v1 source;
    ql_ir_type_definition_v1 definition;
    memset(&source, 0, sizeof(source));
    source.struct_size = sizeof(source);
    status = ql_ir_type_at(state->input, index, &source, error);
    if (status != QL_STATUS_OK) {
      return status;
    }
    ql_ir_type_definition_init(&definition, source.kind);
    definition.float_format = source.float_format;
    definition.bit_width = source.bit_width;
    definition.address_space = source.address_space;
    definition.element_type = source.element_type == QL_IR_INVALID_TYPE_ID
                                  ? QL_IR_INVALID_TYPE_ID
                                  : state->type_map[source.element_type];
    definition.element_count = source.element_count;
    status = ql_ir_builder_add_type(state->builder, &definition,
                                    &state->type_map[index], error);
    if (status != QL_STATUS_OK) {
      return status;
    }
  }
  status = ql_ir_builder_set_function(
      state->builder, state->input_view.function_name,
      state->input_view.function_name_size,
      state->type_map[state->input_view.return_type], error);
  if (status == QL_STATUS_OK) {
    status =
        ql_ir_builder_set_cfg_kind(state->builder, QL_IR_CFG_ACYCLIC, error);
  }
  return status;
}

static ql_ir_type_id find_ir_type(const egraph_method_state *state,
                                  const ql_egraph_type *type, ql_error *error) {
  size_t index;
  for (index = 0u; index < state->input_view.type_count; ++index) {
    ql_ir_type_view_v1 view;
    memset(&view, 0, sizeof(view));
    view.struct_size = sizeof(view);
    if (ql_ir_type_at(state->input, index, &view, error) != QL_STATUS_OK) {
      return QL_IR_INVALID_TYPE_ID;
    }
    if ((type->kind == QL_EGRAPH_SORT_BOOL && view.kind == QL_IR_TYPE_BOOL) ||
        (type->kind == QL_EGRAPH_SORT_BITVECTOR &&
         view.kind == QL_IR_TYPE_BIT_VECTOR &&
         view.bit_width == type->bit_width)) {
      return state->type_map[index];
    }
  }
  ql_error_set(error, QL_STATUS_INTERNAL_ERROR,
               "e-graph term type has no corresponding IR type");
  return QL_IR_INVALID_TYPE_ID;
}

static ql_status add_parameters_and_constants(egraph_method_state *state,
                                              ql_error *error) {
  size_t pass;
  size_t index;
  ql_status status;
  for (pass = 0u; pass < 2u; ++pass) {
    for (index = 0u; index < state->input_view.value_count; ++index) {
      ql_ir_value_view_v1 value;
      memset(&value, 0, sizeof(value));
      value.struct_size = sizeof(value);
      status = ql_ir_value_at(state->input, index, &value, error);
      if (status != QL_STATUS_OK) {
        return status;
      }
      if (pass == 0u && value.definition_kind == QL_IR_VALUE_PARAMETER) {
        status = ql_ir_builder_add_parameter(
            state->builder, state->type_map[value.type], value.name,
            value.name_size, &state->value_map[index], error);
      } else if (pass == 1u && value.definition_kind == QL_IR_VALUE_CONSTANT) {
        status = ql_ir_builder_add_constant(
            state->builder, state->type_map[value.type], value.constant_data,
            value.constant_size, &state->value_map[index], error);
      } else {
        continue;
      }
      if (status != QL_STATUS_OK) {
        return status;
      }
      if (state->value_terms[index] != QL_EGRAPH_INVALID_TERM &&
          state->term_values[state->value_terms[index]] ==
              QL_IR_INVALID_VALUE_ID) {
        state->term_values[state->value_terms[index]] = state->value_map[index];
      }
    }
  }
  for (index = 1u; index <= state->term_count; ++index) {
    ql_egraph_term_view_v1 term;
    ql_ir_type_id type;
    if (state->needed_terms[index] == 0u ||
        state->term_values[index] != QL_IR_INVALID_VALUE_ID) {
      continue;
    }
    memset(&term, 0, sizeof(term));
    status = ql_egraph_get_term(state->graph, (ql_egraph_term_id)index, &term,
                                error);
    if (status != QL_STATUS_OK) {
      return status;
    }
    if (term.op != QL_EGRAPH_OP_BOOL_CONSTANT &&
        term.op != QL_EGRAPH_OP_BV_CONSTANT) {
      continue;
    }
    type = find_ir_type(state, &term.type, error);
    if (type == QL_IR_INVALID_TYPE_ID) {
      return error->code;
    }
    status = ql_ir_builder_add_constant(state->builder, type, term.constant_le,
                                        term.constant_size,
                                        &state->term_values[index], error);
    if (status != QL_STATUS_OK) {
      return status;
    }
  }
  return QL_STATUS_OK;
}

static ql_status append_graph_operation(egraph_method_state *state,
                                        ql_egraph_term_id term_id,
                                        const ql_egraph_term_view_v1 *term,
                                        ql_error *error) {
  ql_ir_instruction_definition_v1 definition;
  ql_ir_value_id operands[3];
  ql_ir_type_id result_type;
  ql_ir_opcode opcode;
  size_t operand;
  ql_ir_instruction_id instruction;

  for (operand = 0u; operand < term->operand_count; ++operand) {
    operands[operand] = state->term_values[term->operands[operand]];
    if (operands[operand] == QL_IR_INVALID_VALUE_ID) {
      return QL_STATUS_NOT_FOUND;
    }
  }
  switch (term->op) {
  case QL_EGRAPH_OP_BOOL_NOT:
    opcode = QL_IR_OPCODE_BOOL_NOT;
    break;
  case QL_EGRAPH_OP_BV_NOT:
    opcode = QL_IR_OPCODE_BV_NOT;
    break;
  case QL_EGRAPH_OP_BV_AND:
    opcode = QL_IR_OPCODE_BV_AND;
    break;
  case QL_EGRAPH_OP_BV_OR:
    opcode = QL_IR_OPCODE_BV_OR;
    break;
  case QL_EGRAPH_OP_BV_XOR:
    opcode = QL_IR_OPCODE_BV_XOR;
    break;
  case QL_EGRAPH_OP_BV_ADD:
    opcode = QL_IR_OPCODE_ADD;
    break;
  case QL_EGRAPH_OP_BV_SUB:
    opcode = QL_IR_OPCODE_SUB;
    break;
  case QL_EGRAPH_OP_BV_MUL:
    opcode = QL_IR_OPCODE_MUL;
    break;
  case QL_EGRAPH_OP_EQUAL:
    opcode = QL_IR_OPCODE_EQ;
    break;
  case QL_EGRAPH_OP_ITE:
    opcode = QL_IR_OPCODE_SELECT;
    break;
  case QL_EGRAPH_OP_BOOL_XOR:
    opcode = QL_IR_OPCODE_NE;
    break;
  case QL_EGRAPH_OP_BOOL_AND:
    opcode = QL_IR_OPCODE_SELECT;
    operands[2] = state->term_values[state->bool_false];
    if (operands[2] == QL_IR_INVALID_VALUE_ID) {
      return QL_STATUS_NOT_FOUND;
    }
    break;
  case QL_EGRAPH_OP_BOOL_OR:
    opcode = QL_IR_OPCODE_SELECT;
    operands[2] = operands[1];
    operands[1] = state->term_values[state->bool_true];
    if (operands[1] == QL_IR_INVALID_VALUE_ID) {
      return QL_STATUS_NOT_FOUND;
    }
    break;
  default:
    ql_error_set(error, QL_STATUS_INTERNAL_ERROR,
                 "extracted e-graph operator %u cannot be emitted",
                 (unsigned)term->op);
    return QL_STATUS_INTERNAL_ERROR;
  }
  result_type = find_ir_type(state, &term->type, error);
  if (result_type == QL_IR_INVALID_TYPE_ID) {
    return error->code;
  }
  ql_ir_instruction_definition_init(&definition, opcode);
  definition.operands = operands;
  definition.operand_count =
      term->op == QL_EGRAPH_OP_BOOL_AND || term->op == QL_EGRAPH_OP_BOOL_OR
          ? 3u
          : term->operand_count;
  definition.result_types = &result_type;
  definition.result_count = 1u;
  return ql_ir_builder_append_instruction(state->builder, state->output_block,
                                          &definition, &instruction,
                                          &state->term_values[term_id], error);
}

static ql_status emit_term(egraph_method_state *state, ql_egraph_term_id root,
                           ql_error *error) {
  size_t depth = 0u;
  state->term_stack[depth++] = root;
  while (depth != 0u) {
    const ql_egraph_term_id term_id = state->term_stack[depth - 1u];
    ql_egraph_term_view_v1 term;
    size_t operand;
    ql_status status;
    if (state->term_values[term_id] != QL_IR_INVALID_VALUE_ID) {
      --depth;
      continue;
    }
    memset(&term, 0, sizeof(term));
    status = ql_egraph_get_term(state->graph, term_id, &term, error);
    if (status != QL_STATUS_OK) {
      return status;
    }
    if (term.op == QL_EGRAPH_OP_VARIABLE ||
        term.op == QL_EGRAPH_OP_BOOL_CONSTANT ||
        term.op == QL_EGRAPH_OP_BV_CONSTANT) {
      return QL_STATUS_NOT_FOUND;
    }
    for (operand = 0u; operand < term.operand_count; ++operand) {
      if (state->term_values[term.operands[operand]] ==
          QL_IR_INVALID_VALUE_ID) {
        if (depth > state->term_count) {
          ql_error_set(error, QL_STATUS_CYCLE, "e-graph extraction term cycle");
          return QL_STATUS_CYCLE;
        }
        state->term_stack[depth++] = term.operands[operand];
        break;
      }
    }
    if (operand != term.operand_count) {
      continue;
    }
    status = append_graph_operation(state, term_id, &term, error);
    if (status != QL_STATUS_OK) {
      return status;
    }
    --depth;
  }
  return QL_STATUS_OK;
}

static ql_status clone_instruction(egraph_method_state *state,
                                   const ql_ir_instruction_view_v1 *source,
                                   ql_error *error) {
  ql_ir_instruction_definition_v1 definition;
  ql_ir_value_id *operands = NULL;
  ql_ir_block_id *blocks = NULL;
  ql_ir_type_id *result_types = NULL;
  ql_ir_value_id *results = NULL;
  ql_ir_instruction_id instruction;
  size_t index;
  ql_status status = QL_STATUS_OK;

  operands =
      allocate_items(state, source->operand_count, sizeof(*operands), error);
  blocks = allocate_items(state, source->block_operand_count, sizeof(*blocks),
                          error);
  result_types =
      allocate_items(state, source->result_count, sizeof(*result_types), error);
  results =
      allocate_items(state, source->result_count, sizeof(*results), error);
  if ((source->operand_count != 0u && operands == NULL) ||
      (source->block_operand_count != 0u && blocks == NULL) ||
      (source->result_count != 0u &&
       (result_types == NULL || results == NULL))) {
    status = QL_STATUS_OUT_OF_MEMORY;
    goto cleanup;
  }
  for (index = 0u; index < source->operand_count; ++index) {
    operands[index] = state->value_map[source->operands[index]];
    if (operands[index] == QL_IR_INVALID_VALUE_ID) {
      ql_error_set(error, QL_STATUS_INTERNAL_ERROR,
                   "IR clone operand is not available");
      status = QL_STATUS_INTERNAL_ERROR;
      goto cleanup;
    }
  }
  for (index = 0u; index < source->block_operand_count; ++index) {
    blocks[index] = state->output_block;
  }
  for (index = 0u; index < source->result_count; ++index) {
    ql_ir_value_view_v1 value;
    memset(&value, 0, sizeof(value));
    value.struct_size = sizeof(value);
    status =
        ql_ir_value_at(state->input, source->results[index], &value, error);
    if (status != QL_STATUS_OK) {
      goto cleanup;
    }
    result_types[index] = state->type_map[value.type];
  }
  ql_ir_instruction_definition_init(&definition, source->opcode);
  definition.flags = source->flags;
  definition.effects = source->effects;
  definition.operands = operands;
  definition.operand_count = source->operand_count;
  definition.block_operands = blocks;
  definition.block_operand_count = source->block_operand_count;
  definition.result_types = result_types;
  definition.result_count = source->result_count;
  definition.immediate = source->immediate;
  definition.symbol = source->symbol;
  definition.symbol_size = source->symbol_size;
  definition.image = source->image;
  definition.image_size = source->image_size;
  status = ql_ir_builder_append_instruction(state->builder, state->output_block,
                                            &definition, &instruction, results,
                                            error);
  if (status != QL_STATUS_OK) {
    goto cleanup;
  }
  for (index = 0u; index < source->result_count; ++index) {
    const ql_ir_value_id old = source->results[index];
    state->value_map[old] = results[index];
    if (state->value_terms[old] != QL_EGRAPH_INVALID_TERM) {
      state->term_values[state->value_terms[old]] = results[index];
    }
  }

cleanup:
  state->instance->allocator.deallocate(state->instance->allocator.user_data,
                                        results);
  state->instance->allocator.deallocate(state->instance->allocator.user_data,
                                        result_types);
  state->instance->allocator.deallocate(state->instance->allocator.user_data,
                                        blocks);
  state->instance->allocator.deallocate(state->instance->allocator.user_data,
                                        operands);
  return status;
}

static ql_status rebuild_instructions(egraph_method_state *state,
                                      const ql_run_context_v1 *context,
                                      ql_error *error) {
  ql_ir_block_view_v1 block;
  size_t index;
  ql_status status;
  memset(&block, 0, sizeof(block));
  block.struct_size = sizeof(block);
  status = ql_ir_block_at(state->input, 0u, &block, error);
  if (status != QL_STATUS_OK) {
    return status;
  }
  for (index = 0u; index < block.instruction_count; ++index) {
    ql_ir_instruction_view_v1 source;
    if (run_cancelled(context)) {
      ql_error_set(error, QL_STATUS_CANCELLED, "%s was cancelled",
                   QL_EGRAPH_METHOD_NAME);
      return QL_STATUS_CANCELLED;
    }
    memset(&source, 0, sizeof(source));
    source.struct_size = sizeof(source);
    status = ql_ir_instruction_at(state->input, block.instructions[index],
                                  &source, error);
    if (status != QL_STATUS_OK) {
      return status;
    }
    if (source.result_count == 1u &&
        state->value_is_normalized[source.results[0]] != 0u) {
      const ql_ir_value_id old = source.results[0];
      status = emit_term(state, state->chosen_terms[old], error);
      if (status != QL_STATUS_OK) {
        return status;
      }
      state->value_map[old] = state->term_values[state->chosen_terms[old]];
    } else {
      status = clone_instruction(state, &source, error);
      if (status != QL_STATUS_OK) {
        return status;
      }
    }
  }
  return QL_STATUS_OK;
}

static ql_ir_value_id mapped_value(const egraph_method_state *state,
                                   ql_ir_value_id value) {
  return value == QL_IR_INVALID_VALUE_ID ? QL_IR_INVALID_VALUE_ID
                                         : state->value_map[value];
}

static ql_status finish_builder(egraph_method_state *state,
                                ql_artifact **output, ql_error *error) {
  ql_ir_block_view_v1 block;
  ql_ir_terminator_definition_v1 terminator;
  ql_status status;
  memset(&block, 0, sizeof(block));
  block.struct_size = sizeof(block);
  status = ql_ir_block_at(state->input, 0u, &block, error);
  if (status != QL_STATUS_OK) {
    return status;
  }
  terminator = block.terminator;
  terminator.condition = mapped_value(state, terminator.condition);
  terminator.return_value = mapped_value(state, terminator.return_value);
  terminator.memory = mapped_value(state, terminator.memory);
  terminator.event_trace = mapped_value(state, terminator.event_trace);
  if (terminator.target != QL_IR_INVALID_BLOCK_ID) {
    terminator.target = state->output_block;
  }
  if (terminator.false_target != QL_IR_INVALID_BLOCK_ID) {
    terminator.false_target = state->output_block;
  }
  status = ql_ir_builder_set_terminator(state->builder, state->output_block,
                                        &terminator, error);
  if (status == QL_STATUS_OK) {
    status = ql_ir_builder_finish(state->builder, output, error);
  }
  return status;
}

static void dispose_state(egraph_method_state *state) {
  ql_allocator *allocator;
  if (state == NULL || state->instance == NULL) {
    return;
  }
  allocator = &state->instance->allocator;
  allocator->deallocate(allocator->user_data, state->term_stack);
  allocator->deallocate(allocator->user_data, state->needed_terms);
  allocator->deallocate(allocator->user_data, state->term_positions);
  allocator->deallocate(allocator->user_data, state->term_sources);
  allocator->deallocate(allocator->user_data, state->term_values);
  allocator->deallocate(allocator->user_data, state->value_map);
  allocator->deallocate(allocator->user_data, state->type_map);
  allocator->deallocate(allocator->user_data, state->value_positions);
  allocator->deallocate(allocator->user_data, state->value_is_normalized);
  allocator->deallocate(allocator->user_data, state->value_is_leaf);
  allocator->deallocate(allocator->user_data, state->chosen_terms);
  allocator->deallocate(allocator->user_data, state->value_terms);
  ql_ir_builder_destroy(state->builder);
  ql_egraph_check_report_release(state->check_report);
  ql_egraph_destroy(state->graph);
  ql_ir_release((ql_ir *)state->input);
  memset(state, 0, sizeof(*state));
}

static ql_status allocate_value_tables(egraph_method_state *state,
                                       ql_error *error) {
  size_t index;
  const size_t values = state->input_view.value_count;
  const size_t types = state->input_view.type_count;
  state->value_terms =
      allocate_items(state, values, sizeof(*state->value_terms), error);
  state->chosen_terms =
      allocate_items(state, values, sizeof(*state->chosen_terms), error);
  state->value_is_leaf =
      allocate_items(state, values, sizeof(*state->value_is_leaf), error);
  state->value_is_normalized =
      allocate_items(state, values, sizeof(*state->value_is_normalized), error);
  state->value_positions =
      allocate_items(state, values, sizeof(*state->value_positions), error);
  state->value_map =
      allocate_items(state, values, sizeof(*state->value_map), error);
  state->type_map =
      allocate_items(state, types, sizeof(*state->type_map), error);
  if ((values != 0u &&
       (state->value_terms == NULL || state->chosen_terms == NULL ||
        state->value_is_leaf == NULL || state->value_is_normalized == NULL ||
        state->value_positions == NULL || state->value_map == NULL)) ||
      (types != 0u && state->type_map == NULL)) {
    return QL_STATUS_OUT_OF_MEMORY;
  }
  memset(state->value_is_leaf, 0, values * sizeof(*state->value_is_leaf));
  memset(state->value_is_normalized, 0,
         values * sizeof(*state->value_is_normalized));
  for (index = 0u; index < values; ++index) {
    state->value_terms[index] = QL_EGRAPH_INVALID_TERM;
    state->chosen_terms[index] = QL_EGRAPH_INVALID_TERM;
    state->value_map[index] = QL_IR_INVALID_VALUE_ID;
  }
  for (index = 0u; index < types; ++index) {
    state->type_map[index] = QL_IR_INVALID_TYPE_ID;
  }
  return initialize_value_positions(state, error);
}

static ql_status verify_output_artifact(ql_artifact *artifact,
                                        ql_error *error) {
  ql_ir *ir = NULL;
  ql_ir_verify_report_v1 report;
  ql_status status = ql_ir_open(NULL, artifact, &ir, error);
  if (status != QL_STATUS_OK) {
    return status;
  }
  ql_ir_verify_report_init(&report);
  status = ql_ir_verify(NULL, ir, &report, error);
  ql_ir_release(ir);
  return status;
}

static ql_status QL_CALL egraph_method_run(
    void *opaque, const ql_run_context_v1 *context, ql_artifact *const *inputs,
    size_t input_count, ql_artifact **output, ql_error *error) {
  egraph_method_instance *instance = (egraph_method_instance *)opaque;
  egraph_method_state state;
  ql_egraph_saturation_result_v1 saturation;
  ql_ir_block_view_v1 input_block;
  ql_artifact *artifact = NULL;
  ql_status status;

  if (instance == NULL || context == NULL || context->host == NULL ||
      output == NULL) {
    ql_error_set(error, QL_STATUS_INVALID_ARGUMENT,
                 "%s requires an instance, run context, and output",
                 QL_EGRAPH_METHOD_NAME);
    return QL_STATUS_INVALID_ARGUMENT;
  }
  *output = NULL;
  memset(&state, 0, sizeof(state));
  state.instance = instance;
  state.bool_false = QL_EGRAPH_INVALID_TERM;
  state.bool_true = QL_EGRAPH_INVALID_TERM;
  status = open_supported_ir(inputs, input_count, (ql_ir **)&state.input,
                             &state.input_view, error);
  if (status != QL_STATUS_OK) {
    goto cleanup;
  }
  if (run_cancelled(context)) {
    ql_error_set(error, QL_STATUS_CANCELLED, "%s was cancelled",
                 QL_EGRAPH_METHOD_NAME);
    status = QL_STATUS_CANCELLED;
    goto cleanup;
  }
  status = allocate_value_tables(&state, error);
  if (status == QL_STATUS_OK) {
    status = build_graph(&state, error);
  }
  if (status != QL_STATUS_OK) {
    goto cleanup;
  }
  memset(&saturation, 0, sizeof(saturation));
  status =
      ql_egraph_saturate(state.graph, &instance->limits, &saturation, error);
  if (status != QL_STATUS_OK) {
    goto cleanup;
  }
  if (run_cancelled(context)) {
    ql_error_set(error, QL_STATUS_CANCELLED, "%s was cancelled",
                 QL_EGRAPH_METHOD_NAME);
    status = QL_STATUS_CANCELLED;
    goto cleanup;
  }
  status = ql_egraph_check_graph(&instance->allocator, state.graph,
                                 &state.check_report, error);
  if (status == QL_STATUS_OK) {
    status = allocate_term_tables(&state, error);
  }
  if (status == QL_STATUS_OK) {
    status = choose_terms(&state, error);
  }
  if (status == QL_STATUS_OK) {
    status = ql_ir_builder_create(&instance->allocator, &state.builder, error);
  }
  if (status == QL_STATUS_OK) {
    status = clone_types_and_function(&state, error);
  }
  if (status == QL_STATUS_OK) {
    status = add_parameters_and_constants(&state, error);
  }
  memset(&input_block, 0, sizeof(input_block));
  input_block.struct_size = sizeof(input_block);
  if (status == QL_STATUS_OK) {
    status = ql_ir_block_at(state.input, 0u, &input_block, error);
  }
  if (status == QL_STATUS_OK) {
    status = ql_ir_builder_add_block(state.builder, input_block.label,
                                     input_block.label_size,
                                     &state.output_block, error);
  }
  if (status == QL_STATUS_OK) {
    status =
        ql_ir_builder_set_entry_block(state.builder, state.output_block, error);
  }
  if (status == QL_STATUS_OK) {
    status = rebuild_instructions(&state, context, error);
  }
  if (status == QL_STATUS_OK) {
    status = finish_builder(&state, &artifact, error);
  }
  if (status == QL_STATUS_OK) {
    status = verify_output_artifact(artifact, error);
  }
  if (status == QL_STATUS_OK) {
    *output = artifact;
    artifact = NULL;
    ql_error_clear(error);
  }

cleanup:
  ql_artifact_release(artifact);
  dispose_state(&state);
  return status;
}

static const ql_method_v1 egraph_method = {
    sizeof(ql_method_v1),
    QL_ABI_VERSION,
    QL_EGRAPH_METHOD_NAME,
    "Normalize one verified straight-line IR artifact with independently "
    "replayed e-graph rewrites",
    QL_ARTIFACT_KIND_IR,
    QL_METHOD_DETERMINISTIC | QL_METHOD_CACHEABLE,
    1u,
    1u,
    egraph_method_create,
    egraph_method_validate,
    egraph_method_run,
    egraph_method_destroy,
    {NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL}};

ql_status QL_CALL ql_register_egraph_method(ql_registry *registry,
                                            ql_error *error) {
  return ql_registry_register(registry, &egraph_method, error);
}
