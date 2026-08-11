#include "quodlibet/proof_egraph.h"

#include <inttypes.h>
#include <stdio.h>
#include <string.h>

#include "quodlibet/c_lower.h"
#include "quodlibet/egraph.h"
#include "quodlibet/egraph_check.h"
#include "quodlibet/log.h"
#include "quodlibet/problem.h"

#include "yyjson.h"

#define QL_EGRAPH_PROOF_CATEGORY "proof.egraph"

typedef struct egraph_proof_instance {
    ql_allocator allocator;
    uint64_t node_limit;
    uint32_t iteration_limit;
    uint64_t rewrite_limit;
} egraph_proof_instance;

typedef struct egraph_proof_decision {
    ql_verdict verdict;
    ql_evidence_class evidence_class;
    uint32_t checked_proof;
    uint32_t saturation_complete;
    uint64_t merge_count;
    uint64_t justified_count;
    uint64_t assumed_count;
    uint64_t rejected_count;
    uint64_t term_count;
    char diagnostic[QL_ERROR_MESSAGE_CAPACITY];
} egraph_proof_decision;

/* --- yyjson glue ---------------------------------------------------------- */

static void *proof_json_allocate(void *context, size_t size) {
    ql_allocator *allocator = (ql_allocator *)context;
    return allocator->allocate(allocator->user_data, size);
}

static void *proof_json_reallocate(void *context, void *pointer,
                                   size_t old_size, size_t size) {
    ql_allocator *allocator = (ql_allocator *)context;
    (void)old_size;
    return allocator->reallocate(allocator->user_data, pointer, size);
}

static void proof_json_deallocate(void *context, void *pointer) {
    ql_allocator *allocator = (ql_allocator *)context;
    allocator->deallocate(allocator->user_data, pointer);
}

static yyjson_alc proof_json_allocator(ql_allocator *allocator) {
    yyjson_alc result;
    result.malloc = proof_json_allocate;
    result.realloc = proof_json_reallocate;
    result.free = proof_json_deallocate;
    result.ctx = allocator;
    return result;
}

static void set_diagnostic(egraph_proof_decision *decision,
                           const char *message) {
    size_t length = strlen(message);
    if (length >= sizeof(decision->diagnostic)) {
        length = sizeof(decision->diagnostic) - 1u;
    }
    memcpy(decision->diagnostic, message, length);
    decision->diagnostic[length] = '\0';
}

static void set_diagnostic_format(egraph_proof_decision *decision,
                                  const char *format, uint64_t value) {
    int written = snprintf(decision->diagnostic, sizeof(decision->diagnostic),
                           format, (unsigned long long)value);
    if (written < 0) {
        decision->diagnostic[0] = '\0';
    }
}

/* --- Options -------------------------------------------------------------- */

static ql_status parse_options(egraph_proof_instance *instance,
                               const char *options_json, ql_error *error) {
    static const char *const known[] = {"node_limit", "iteration_limit",
                                        "rewrite_limit"};
    yyjson_alc json_allocator = proof_json_allocator(&instance->allocator);
    yyjson_doc *document;
    yyjson_read_err read_error;
    yyjson_val *root;
    yyjson_val *value;
    yyjson_obj_iter iterator;
    yyjson_val *key;
    ql_status status = QL_STATUS_OK;

    if (options_json == NULL || options_json[0] == '\0') {
        return QL_STATUS_OK;
    }
    document = yyjson_read_opts((char *)(uintptr_t)options_json,
                                strlen(options_json), 0u, &json_allocator,
                                &read_error);
    if (document == NULL) {
        ql_error_set(error, QL_STATUS_PARSE_ERROR,
                     "invalid %s options at byte %zu: %s",
                     QL_EGRAPH_PROOF_METHOD_NAME, read_error.pos,
                     read_error.msg != NULL ? read_error.msg : "parse error");
        return QL_STATUS_PARSE_ERROR;
    }
    root = yyjson_doc_get_root(document);
    if (!yyjson_is_obj(root)) {
        ql_error_set(error, QL_STATUS_PARSE_ERROR,
                     "%s options must be a JSON object",
                     QL_EGRAPH_PROOF_METHOD_NAME);
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
                         "%s does not accept the option '%s'",
                         QL_EGRAPH_PROOF_METHOD_NAME,
                         name != NULL ? name : "");
            status = QL_STATUS_INVALID_ARGUMENT;
            goto cleanup;
        }
    }

    value = yyjson_obj_get(root, "node_limit");
    if (value != NULL) {
        if (!yyjson_is_uint(value) || yyjson_get_uint(value) == 0u) {
            ql_error_set(error, QL_STATUS_INVALID_ARGUMENT,
                         "node_limit must be a positive integer");
            status = QL_STATUS_INVALID_ARGUMENT;
            goto cleanup;
        }
        instance->node_limit = yyjson_get_uint(value);
    }
    value = yyjson_obj_get(root, "iteration_limit");
    if (value != NULL) {
        if (!yyjson_is_uint(value) || yyjson_get_uint(value) == 0u ||
            yyjson_get_uint(value) > UINT32_MAX) {
            ql_error_set(error, QL_STATUS_INVALID_ARGUMENT,
                         "iteration_limit must be a positive 32-bit integer");
            status = QL_STATUS_INVALID_ARGUMENT;
            goto cleanup;
        }
        instance->iteration_limit = (uint32_t)yyjson_get_uint(value);
    }
    value = yyjson_obj_get(root, "rewrite_limit");
    if (value != NULL) {
        if (!yyjson_is_uint(value) || yyjson_get_uint(value) == 0u) {
            ql_error_set(error, QL_STATUS_INVALID_ARGUMENT,
                         "rewrite_limit must be a positive integer");
            status = QL_STATUS_INVALID_ARGUMENT;
            goto cleanup;
        }
        instance->rewrite_limit = yyjson_get_uint(value);
    }

cleanup:
    yyjson_doc_free(document);
    return status;
}

/* --- Capability ----------------------------------------------------------- */

static ql_status QL_CALL egraph_proof_capability(
    const char *options_json, ql_proof_method_capability_v1 *capability,
    ql_error *error) {
    egraph_proof_instance probe;
    ql_status status;

    memset(&probe, 0, sizeof(probe));
    probe.allocator = *ql_default_allocator();
    status = parse_options(&probe, options_json, error);
    if (status != QL_STATUS_OK) {
        return status;
    }
    capability->family = QL_PROOF_METHOD_FAMILY_REWRITE_EGRAPH;
    capability->soundness_classes = QL_PROOF_SOUNDNESS_PROOF;
    capability->result_kinds =
        QL_PROOF_RESULT_PROOF | QL_PROOF_RESULT_UNKNOWN;
    /* Equality on every input discharges every direction at once inside the
       total, deterministic, defined-everywhere fragment this method accepts. */
    capability->supported_relations = QL_PROOF_RELATION_ALL;
    capability->supported_ub_policies = QL_PROOF_UB_ALL;
    capability->supported_observations = QL_OBSERVE_ALL;
    capability->supported_memory_observations =
        QL_PROOF_MEMORY_MODE(QL_MEMORY_IGNORE) |
        QL_PROOF_MEMORY_MODE(QL_MEMORY_FINAL_REACHABLE_STATE) |
        QL_PROOF_MEMORY_MODE(QL_MEMORY_ORDERED_WRITES) |
        QL_PROOF_MEMORY_MODE(QL_MEMORY_FULL_TRACE);
    capability->supported_external_call_observations =
        QL_PROOF_EXTERNAL_CALL_MODE(QL_EXTERNAL_CALLS_IGNORE) |
        QL_PROOF_EXTERNAL_CALL_MODE(QL_EXTERNAL_CALLS_ORDERED_TRACE);
    capability->flags = QL_PROOF_CAPABILITY_PRECONDITIONS;
    ql_error_clear(error);
    return QL_STATUS_OK;
}

/* --- Lowering ------------------------------------------------------------- */

typedef struct egraph_proof_side {
    ql_c_frontend_unit *unit;
    ql_c_lower_result *result;
    ql_ir *ir;
    ql_ir_view_v1 view;
} egraph_proof_side;

static void egraph_proof_side_dispose(egraph_proof_side *side) {
    ql_ir_release(side->ir);
    ql_c_lower_result_destroy(side->result);
    ql_c_frontend_unit_destroy(side->unit);
    memset(side, 0, sizeof(*side));
}

static ql_status lower_side(const ql_allocator *allocator, const char *source,
                            size_t source_size, const char *name,
                            size_t name_size, egraph_proof_side *side,
                            uint32_t *supported, ql_error *error) {
    ql_c_function_view function;
    ql_c_lower_result_view_v1 view;
    ql_status status;

    memset(side, 0, sizeof(*side));
    *supported = 0u;
    status = ql_c_frontend_analyze(allocator, source, source_size, &side->unit,
                                   error);
    if (status != QL_STATUS_OK) {
        return status;
    }
    memset(&function, 0, sizeof(function));
    function.struct_size = sizeof(function);
    status = ql_c_frontend_select_function(side->unit, name, name_size,
                                           &function, error);
    if (status != QL_STATUS_OK) {
        return status;
    }
    status = ql_c_lower_selected_function(allocator, source, source_size,
                                          side->unit, &function, &side->result,
                                          error);
    if (status != QL_STATUS_OK) {
        return status;
    }
    memset(&view, 0, sizeof(view));
    view.struct_size = sizeof(view);
    status = ql_c_lower_result_get_view(side->result, &view, error);
    if (status != QL_STATUS_OK) {
        return status;
    }
    if (view.support != QL_C_LOWER_SUPPORTED || view.ir_artifact == NULL) {
        return QL_STATUS_OK;
    }
    status = ql_ir_open(allocator, view.ir_artifact, &side->ir, error);
    if (status != QL_STATUS_OK) {
        return status;
    }
    side->view.struct_size = sizeof(side->view);
    status = ql_ir_get_view(side->ir, &side->view, error);
    if (status != QL_STATUS_OK) {
        return status;
    }
    *supported = 1u;
    return QL_STATUS_OK;
}

/* --- IR to terms ---------------------------------------------------------- */

typedef struct term_builder {
    const ql_allocator *allocator;
    ql_egraph *graph;
    const egraph_proof_side *side;
    /* One term per IR value id; parameters are seeded from the shared
       variable table before instructions run. */
    ql_egraph_term_id *value_terms;
} term_builder;

/* A rejection here is a statement about this method's fragment, so it lands
   in the decision's diagnostic and the run reports UNKNOWN. */
static ql_status fragment_reject(egraph_proof_decision *decision,
                                 const char *message) {
    set_diagnostic(decision, message);
    return QL_STATUS_TYPE_MISMATCH;
}

static ql_status side_type_at(const egraph_proof_side *side,
                              ql_ir_type_id type_id,
                              ql_ir_type_view_v1 *type, ql_error *error) {
    memset(type, 0, sizeof(*type));
    type->struct_size = sizeof(*type);
    return ql_ir_type_at(side->ir, type_id, type, error);
}

static ql_status value_type_view(const egraph_proof_side *side,
                                 ql_ir_value_id value,
                                 ql_ir_type_view_v1 *type, ql_error *error) {
    ql_ir_value_view_v1 value_view;
    ql_status status;

    memset(&value_view, 0, sizeof(value_view));
    value_view.struct_size = sizeof(value_view);
    status = ql_ir_value_at(side->ir, value, &value_view, error);
    if (status != QL_STATUS_OK) {
        return status;
    }
    return side_type_at(side, value_view.type, type, error);
}

static int type_is_graphable(const ql_ir_type_view_v1 *type,
                             uint32_t max_bit_width) {
    return type->kind == QL_IR_TYPE_BOOL ||
           (type->kind == QL_IR_TYPE_BIT_VECTOR && type->bit_width != 0u &&
            type->bit_width <= max_bit_width);
}

/* Collects each side's parameter value ids in declaration order, which is the
   order the source signature and therefore the argument correspondence
   speaks in. */
static ql_status collect_parameters(const egraph_proof_side *side,
                                    ql_ir_value_id *parameters,
                                    size_t capacity, size_t *count,
                                    ql_error *error) {
    size_t index;

    *count = 0u;
    for (index = 0u; index < side->view.value_count; ++index) {
        ql_ir_value_view_v1 value;
        ql_status status;
        memset(&value, 0, sizeof(value));
        value.struct_size = sizeof(value);
        status = ql_ir_value_at(side->ir, index, &value, error);
        if (status != QL_STATUS_OK) {
            return status;
        }
        if (value.definition_kind != QL_IR_VALUE_PARAMETER) {
            continue;
        }
        if (*count >= capacity) {
            ql_error_set(error, QL_STATUS_TYPE_MISMATCH,
                         "too many parameters for the e-graph proof fragment");
            return QL_STATUS_TYPE_MISMATCH;
        }
        parameters[(*count)++] = value.id;
    }
    return QL_STATUS_OK;
}

static ql_status seed_constants(term_builder *builder,
                                uint32_t max_bit_width, ql_error *error) {
    const egraph_proof_side *side = builder->side;
    size_t index;

    for (index = 0u; index < side->view.value_count; ++index) {
        ql_ir_value_view_v1 value;
        ql_ir_type_view_v1 type;
        ql_status status;
        memset(&value, 0, sizeof(value));
        value.struct_size = sizeof(value);
        status = ql_ir_value_at(side->ir, index, &value, error);
        if (status != QL_STATUS_OK) {
            return status;
        }
        if (value.definition_kind != QL_IR_VALUE_CONSTANT) {
            continue;
        }
        status = side_type_at(side, value.type, &type, error);
        if (status != QL_STATUS_OK) {
            return status;
        }
        if (!type_is_graphable(&type, max_bit_width)) {
            /* Left invalid; a use will be refused with a named diagnostic. */
            continue;
        }
        if (type.kind == QL_IR_TYPE_BOOL) {
            const uint8_t *byte = (const uint8_t *)value.constant_data;
            status = ql_egraph_make_bool_constant(
                builder->graph, byte != NULL ? byte[0] : 0u,
                &builder->value_terms[value.id], error);
        } else {
            status = ql_egraph_make_bv_constant(
                builder->graph, type.bit_width,
                (const uint8_t *)value.constant_data, value.constant_size,
                &builder->value_terms[value.id], error);
        }
        if (status != QL_STATUS_OK) {
            return status;
        }
    }
    return QL_STATUS_OK;
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

/* Converts every instruction of the side's single block, strictly: anything
   the pure term vocabulary cannot carry is a fragment rejection, not an
   opaque leaf. An unshared leaf can never merge across sides, so admitting
   one would only manufacture UNKNOWNs that look like near-misses. */
static ql_status convert_block(term_builder *builder,
                               egraph_proof_decision *decision,
                               ql_error *error) {
    const egraph_proof_side *side = builder->side;
    ql_ir_block_view_v1 block;
    size_t index;
    ql_status status;

    memset(&block, 0, sizeof(block));
    block.struct_size = sizeof(block);
    status = ql_ir_block_at(side->ir, side->view.entry_block, &block, error);
    if (status != QL_STATUS_OK) {
        return status;
    }
    for (index = 0u; index < block.instruction_count; ++index) {
        ql_ir_instruction_view_v1 view;
        ql_egraph_term_id operands[QL_EGRAPH_MAX_ARITY];
        ql_egraph_operator op;
        size_t operand;

        memset(&view, 0, sizeof(view));
        view.struct_size = sizeof(view);
        status = ql_ir_instruction_at(side->ir, block.instructions[index],
                                      &view, error);
        if (status != QL_STATUS_OK) {
            return status;
        }
        if (view.effects != QL_IR_EFFECT_NONE) {
            return fragment_reject(decision,
                                   "an effectful instruction is outside the pure term fragment");
        }
        if (view.opcode == QL_IR_OPCODE_UB_GUARD ||
            view.opcode == QL_IR_OPCODE_ASSUME) {
            return fragment_reject(decision,
                                   "a definedness guard or assumption is outside the pure term fragment");
        }
        if (view.result_count != 1u || view.block_operand_count != 0u ||
            view.operand_count > QL_EGRAPH_MAX_ARITY) {
            return fragment_reject(decision,
                                   "an instruction shape is outside the pure term fragment");
        }
        for (operand = 0u; operand < view.operand_count; ++operand) {
            operands[operand] = builder->value_terms[view.operands[operand]];
            if (operands[operand] == QL_EGRAPH_INVALID_TERM) {
                return fragment_reject(decision,
                                       "an operand's type is outside the pure term fragment");
            }
        }
        if (view.opcode == QL_IR_OPCODE_IDENTITY &&
            view.operand_count == 1u) {
            builder->value_terms[view.results[0]] = operands[0];
            continue;
        }
        if (view.opcode == QL_IR_OPCODE_BV_NEG && view.operand_count == 1u) {
            ql_ir_type_view_v1 type;
            ql_egraph_term_id zero;
            status = value_type_view(side, view.results[0], &type, error);
            if (status != QL_STATUS_OK) {
                return status;
            }
            status = ql_egraph_make_bv_u64(builder->graph, type.bit_width, 0u,
                                           &zero, error);
            if (status != QL_STATUS_OK) {
                return status;
            }
            operands[1] = operands[0];
            operands[0] = zero;
            status = ql_egraph_make_operation(
                builder->graph, QL_EGRAPH_OP_BV_SUB, operands, 2u,
                &builder->value_terms[view.results[0]], error);
            if (status != QL_STATUS_OK) {
                return status;
            }
            continue;
        }
        if (view.opcode == QL_IR_OPCODE_NE && view.operand_count == 2u) {
            ql_egraph_term_id equal;
            status = ql_egraph_make_operation(builder->graph,
                                              QL_EGRAPH_OP_EQUAL, operands,
                                              2u, &equal, error);
            if (status != QL_STATUS_OK) {
                return status;
            }
            status = ql_egraph_make_operation(
                builder->graph, QL_EGRAPH_OP_BOOL_NOT, &equal, 1u,
                &builder->value_terms[view.results[0]], error);
            if (status != QL_STATUS_OK) {
                return status;
            }
            continue;
        }
        op = graph_operator(view.opcode);
        if (op == QL_EGRAPH_OP_INVALID ||
            !ql_egraph_operator_is_supported(op)) {
            set_diagnostic_format(
                decision,
                "IR opcode %llu has no pure term operator, so the pair is outside prove.egraph",
                view.opcode);
            return QL_STATUS_TYPE_MISMATCH;
        }
        status = ql_egraph_make_operation(
            builder->graph, op, operands, view.operand_count,
            &builder->value_terms[view.results[0]], error);
        if (status != QL_STATUS_OK) {
            return status;
        }
    }
    return QL_STATUS_OK;
}

/* Structural gates on one side: a single block that returns a graphable
   scalar and observes nothing else. */
static ql_status check_shape(const egraph_proof_side *side,
                             uint32_t max_bit_width,
                             egraph_proof_decision *decision,
                             ql_ir_value_id *return_value, ql_error *error) {
    ql_ir_block_view_v1 block;
    ql_ir_type_view_v1 return_type;
    ql_status status;

    if (side->view.block_count != 1u) {
        return fragment_reject(decision,
                               "control flow is outside the e-graph proof fragment; one basic block is required");
    }
    memset(&block, 0, sizeof(block));
    block.struct_size = sizeof(block);
    status = ql_ir_block_at(side->ir, side->view.entry_block, &block, error);
    if (status != QL_STATUS_OK) {
        return status;
    }
    if (block.terminator.kind != QL_IR_TERMINATOR_RETURN ||
        block.terminator.return_value == QL_IR_INVALID_VALUE_ID) {
        return fragment_reject(decision,
                               "the fragment requires a single RETURN of a value");
    }
    if (block.terminator.memory != QL_IR_INVALID_VALUE_ID ||
        block.terminator.event_trace != QL_IR_INVALID_VALUE_ID) {
        return fragment_reject(decision,
                               "a memory or event observation is outside the pure term fragment");
    }
    status = value_type_view(side, block.terminator.return_value,
                             &return_type, error);
    if (status != QL_STATUS_OK) {
        return status;
    }
    if (!type_is_graphable(&return_type, max_bit_width)) {
        return fragment_reject(decision,
                               "the return type is outside the pure term fragment");
    }
    *return_value = block.terminator.return_value;
    return QL_STATUS_OK;
}

/* --- Cache key and outcome ------------------------------------------------ */

static int add_digest(yyjson_mut_doc *document, yyjson_mut_val *object,
                      const char *key, const ql_digest *digest) {
    char hex[QL_DIGEST_HEX_SIZE];
    ql_digest_hex(digest, hex);
    return yyjson_mut_obj_add_strcpy(document, object, key, hex);
}

static ql_status compute_cache_key(const ql_digest *problem_digest,
                                   const egraph_proof_instance *instance,
                                   const ql_digest *catalogue_digest,
                                   ql_digest *cache_key, ql_error *error) {
    /* The limits bound the saturation, so a different limit is a different
       search; the catalogue digest is the rewrite-set identity. */
    char identity[384];
    char catalogue_hex[QL_DIGEST_HEX_SIZE];
    ql_cache_key_input_v1 input;
    int written;

    ql_digest_hex(catalogue_digest, catalogue_hex);
    written = snprintf(
        identity, sizeof(identity),
        "{\"node_limit\":%llu,\"iteration_limit\":%u,\"rewrite_limit\":%llu,\"catalogue\":\"%s\"}",
        (unsigned long long)instance->node_limit,
        (unsigned)instance->iteration_limit,
        (unsigned long long)instance->rewrite_limit, catalogue_hex);
    if (written < 0 || (size_t)written >= sizeof(identity)) {
        ql_error_set(error, QL_STATUS_INTERNAL_ERROR,
                     "could not format the cache identity");
        return QL_STATUS_INTERNAL_ERROR;
    }
    memset(&input, 0, sizeof(input));
    input.struct_size = sizeof(input);
    input.artifact_digest = *problem_digest;
    input.semantic_problem_digest = *problem_digest;
    input.method_name = QL_EGRAPH_PROOF_METHOD_NAME;
    input.method_version = QL_EGRAPH_PROOF_METHOD_VERSION;
    input.canonical_options = identity;
    input.canonical_options_size = (size_t)written;
    return ql_cache_key_compute(&input, cache_key, error);
}

static ql_status build_outcome(const ql_allocator *allocator,
                               const egraph_proof_instance *instance,
                               const ql_problem_view_v2 *problem_view,
                               const egraph_proof_decision *decision,
                               ql_artifact **output, ql_error *error) {
    ql_allocator allocator_copy = *allocator;
    yyjson_alc json_allocator = proof_json_allocator(&allocator_copy);
    yyjson_mut_doc *document = NULL;
    yyjson_mut_val *root;
    yyjson_mut_val *saturation_object;
    yyjson_mut_val *replay_object;
    yyjson_mut_val *trust_object;
    yyjson_write_err write_error;
    ql_digest catalogue_digest;
    ql_digest cache_key;
    char *json = NULL;
    size_t json_size = 0u;
    ql_status status;

    status = ql_egraph_rule_catalogue_digest(&catalogue_digest, error);
    if (status != QL_STATUS_OK) {
        return status;
    }
    status = compute_cache_key(&problem_view->artifact_digest, instance,
                               &catalogue_digest, &cache_key, error);
    if (status != QL_STATUS_OK) {
        return status;
    }

    document = yyjson_mut_doc_new(&json_allocator);
    root = document != NULL ? yyjson_mut_obj(document) : NULL;
    saturation_object = document != NULL ? yyjson_mut_obj(document) : NULL;
    replay_object = document != NULL ? yyjson_mut_obj(document) : NULL;
    trust_object = document != NULL ? yyjson_mut_obj(document) : NULL;
    if (document == NULL || root == NULL || saturation_object == NULL ||
        replay_object == NULL || trust_object == NULL ||
        !yyjson_mut_obj_add_str(document, root, "kind",
                                QL_ARTIFACT_KIND_OUTCOME) ||
        !yyjson_mut_obj_add_uint(document, root, "schema_version",
                                 QL_EGRAPH_PROOF_OUTCOME_SCHEMA_VERSION) ||
        !yyjson_mut_obj_add_str(document, root, "method",
                                QL_EGRAPH_PROOF_METHOD_NAME) ||
        !yyjson_mut_obj_add_str(document, root, "method_version",
                                QL_EGRAPH_PROOF_METHOD_VERSION) ||
        !yyjson_mut_obj_add_str(document, root, "verdict",
                                ql_verdict_string(decision->verdict)) ||
        !yyjson_mut_obj_add_str(
            document, root, "evidence_class",
            ql_evidence_class_string(decision->evidence_class)) ||
        !add_digest(document, root, "problem_digest",
                    &problem_view->artifact_digest) ||
        !add_digest(document, root, "cache_key", &cache_key) ||
        !yyjson_mut_obj_add_strcpy(document, root, "diagnostic",
                                   decision->diagnostic)) {
        status = QL_STATUS_OUT_OF_MEMORY;
        ql_error_set(error, status, NULL);
        goto cleanup;
    }
    if (!yyjson_mut_obj_add_bool(document, saturation_object, "complete",
                                 decision->saturation_complete != 0u) ||
        !yyjson_mut_obj_add_uint(document, saturation_object, "terms",
                                 decision->term_count) ||
        !yyjson_mut_obj_add_uint(document, saturation_object, "merges",
                                 decision->merge_count) ||
        !yyjson_mut_obj_add_val(document, root, "saturation",
                                saturation_object)) {
        status = QL_STATUS_OUT_OF_MEMORY;
        ql_error_set(error, status, NULL);
        goto cleanup;
    }
    if (!yyjson_mut_obj_add_uint(document, replay_object, "justified",
                                 decision->justified_count) ||
        !yyjson_mut_obj_add_uint(document, replay_object, "assumed",
                                 decision->assumed_count) ||
        !yyjson_mut_obj_add_uint(document, replay_object, "rejected",
                                 decision->rejected_count) ||
        !yyjson_mut_obj_add_uint(document, replay_object,
                                 "rule_catalogue_version",
                                 QL_EGRAPH_RULE_CATALOGUE_VERSION) ||
        !add_digest(document, replay_object, "rule_catalogue_digest",
                    &catalogue_digest) ||
        !yyjson_mut_obj_add_val(document, root, "replay", replay_object)) {
        status = QL_STATUS_OUT_OF_MEMORY;
        ql_error_set(error, status, NULL);
        goto cleanup;
    }
    if (!yyjson_mut_obj_add_bool(document, trust_object, "checked_proof",
                                 decision->checked_proof != 0u) ||
        !yyjson_mut_obj_add_bool(document, trust_object, "replay_confirmed",
                                 0) ||
        !yyjson_mut_obj_add_str(
            document, trust_object, "basis",
            decision->checked_proof != 0u
                ? "the independent merge-log replay justified every merge and its own replayed state puts both return roots in one class"
                : "no sound conclusion was reached; failure to merge is incompleteness, never inequality") ||
        !yyjson_mut_obj_add_val(document, root, "trust", trust_object)) {
        status = QL_STATUS_OUT_OF_MEMORY;
        ql_error_set(error, status, NULL);
        goto cleanup;
    }

    yyjson_mut_doc_set_root(document, root);
    json = yyjson_mut_write_opts(document, 0u, &json_allocator, &json_size,
                                 &write_error);
    if (json == NULL) {
        status = write_error.code == YYJSON_WRITE_ERROR_MEMORY_ALLOCATION
                     ? QL_STATUS_OUT_OF_MEMORY
                     : QL_STATUS_PARSE_ERROR;
        ql_error_set(error, status, "cannot serialize the outcome JSON: %s",
                     write_error.msg != NULL ? write_error.msg
                                             : "JSON write error");
        goto cleanup;
    }
    status = ql_artifact_create(allocator, QL_ARTIFACT_KIND_OUTCOME,
                                QL_EGRAPH_PROOF_OUTCOME_SCHEMA_VERSION, json,
                                json_size, output, error);

cleanup:
    if (json != NULL) {
        json_allocator.free(json_allocator.ctx, json);
    }
    if (document != NULL) {
        yyjson_mut_doc_free(document);
    }
    return status;
}

/* --- Method callbacks ----------------------------------------------------- */

static ql_status QL_CALL egraph_proof_create(const ql_host_v1 *host,
                                             const char *options_json,
                                             void **instance,
                                             ql_error *error) {
    egraph_proof_instance *created;
    ql_allocator allocator;
    ql_egraph_config_v1 config;
    ql_egraph_saturation_limits_v1 limits;
    ql_status status;

    if (host == NULL || instance == NULL) {
        ql_error_set(error, QL_STATUS_INVALID_ARGUMENT,
                     "host and instance output are required");
        return QL_STATUS_INVALID_ARGUMENT;
    }
    *instance = NULL;
    allocator = host->allocator;
    if (!ql_allocator_is_valid(&allocator)) {
        allocator = *ql_default_allocator();
    }
    created = allocator.allocate(allocator.user_data, sizeof(*created));
    if (created == NULL) {
        ql_error_set(error, QL_STATUS_OUT_OF_MEMORY, NULL);
        return QL_STATUS_OUT_OF_MEMORY;
    }
    memset(created, 0, sizeof(*created));
    created->allocator = allocator;
    ql_egraph_config_init(&config);
    ql_egraph_saturation_limits_init(&limits);
    created->node_limit = config.max_terms;
    created->iteration_limit = limits.max_iterations;
    created->rewrite_limit = limits.max_rewrite_applications;
    status = parse_options(created, options_json, error);
    if (status != QL_STATUS_OK) {
        allocator.deallocate(allocator.user_data, created);
        return status;
    }
    *instance = created;
    ql_error_clear(error);
    return QL_STATUS_OK;
}

static void QL_CALL egraph_proof_destroy(void *instance) {
    egraph_proof_instance *owned = (egraph_proof_instance *)instance;
    ql_allocator allocator;

    if (owned == NULL) {
        return;
    }
    allocator = owned->allocator;
    allocator.deallocate(allocator.user_data, owned);
}

static ql_status QL_CALL egraph_proof_validate(void *instance,
                                               ql_artifact *const *inputs,
                                               size_t input_count,
                                               ql_error *error) {
    ql_artifact_view view;
    ql_status status;

    (void)instance;
    if (inputs == NULL || input_count != 1u || inputs[0] == NULL) {
        ql_error_set(error, QL_STATUS_INVALID_ARGUMENT,
                     "%s consumes exactly one quodlibet.problem artifact",
                     QL_EGRAPH_PROOF_METHOD_NAME);
        return QL_STATUS_INVALID_ARGUMENT;
    }
    memset(&view, 0, sizeof(view));
    view.struct_size = sizeof(view);
    status = ql_artifact_get_view(inputs[0], &view, error);
    if (status != QL_STATUS_OK) {
        return status;
    }
    if (strcmp(view.kind, QL_ARTIFACT_KIND_PROBLEM) != 0) {
        ql_error_set(error, QL_STATUS_TYPE_MISMATCH,
                     "%s input is not a quodlibet.problem",
                     QL_EGRAPH_PROOF_METHOD_NAME);
        return QL_STATUS_TYPE_MISMATCH;
    }
    if (view.schema_version != QL_PROBLEM_SCHEMA_VERSION_2) {
        ql_error_set(error, QL_STATUS_TYPE_MISMATCH,
                     "%s requires problem schema v2; schema v%u records no argument correspondence",
                     QL_EGRAPH_PROOF_METHOD_NAME, view.schema_version);
        return QL_STATUS_TYPE_MISMATCH;
    }
    ql_error_clear(error);
    return QL_STATUS_OK;
}

static ql_verdict proved_verdict_for(ql_relation relation) {
    switch (relation) {
    case QL_RELATION_LEFT_REFINES_RIGHT:
        return QL_VERDICT_PROVED_LEFT_REFINES_RIGHT;
    case QL_RELATION_RIGHT_REFINES_LEFT:
        return QL_VERDICT_PROVED_RIGHT_REFINES_LEFT;
    default:
        return QL_VERDICT_PROVED_EQUIVALENT;
    }
}

#define QL_EGRAPH_PROOF_MAX_PARAMETERS 64u

static ql_status QL_CALL egraph_proof_run(void *instance,
                                          const ql_run_context_v1 *context,
                                          ql_artifact *const *inputs,
                                          size_t input_count,
                                          ql_artifact **output,
                                          ql_error *error) {
    egraph_proof_instance *owned = (egraph_proof_instance *)instance;
    const ql_allocator *allocator;
    ql_problem *problem = NULL;
    ql_problem_view_v2 problem_view;
    ql_egraph *graph = NULL;
    ql_egraph_check_report *report = NULL;
    egraph_proof_side left;
    egraph_proof_side right;
    egraph_proof_decision decision;
    ql_egraph_term_id *left_terms = NULL;
    ql_egraph_term_id *right_terms = NULL;
    uint32_t left_supported = 0u;
    uint32_t right_supported = 0u;
    ql_status status;

    memset(&left, 0, sizeof(left));
    memset(&right, 0, sizeof(right));
    memset(&decision, 0, sizeof(decision));
    decision.verdict = QL_VERDICT_UNKNOWN;
    decision.evidence_class = QL_EVIDENCE_UNKNOWN;
    if (owned == NULL || context == NULL || output == NULL) {
        ql_error_set(error, QL_STATUS_INVALID_ARGUMENT,
                     "instance, run context, and output are required");
        return QL_STATUS_INVALID_ARGUMENT;
    }
    *output = NULL;
    status = egraph_proof_validate(instance, inputs, input_count, error);
    if (status != QL_STATUS_OK) {
        return status;
    }
    allocator = &owned->allocator;
    status = ql_problem_open(allocator, inputs[0], &problem, error);
    if (status != QL_STATUS_OK) {
        return status;
    }
    memset(&problem_view, 0, sizeof(problem_view));
    problem_view.struct_size = sizeof(problem_view);
    status = ql_problem_get_view_v2(problem, &problem_view, error);
    if (status != QL_STATUS_OK) {
        goto cleanup;
    }

    /* Every gate below that leaves the fragment produces an UNKNOWN outcome
       with its diagnostic; only transport failures leave as status errors. */
    if (problem_view.contract.precondition_json != NULL) {
        set_diagnostic(&decision,
                       "a typed precondition needs a domain inhabitation witness this method does not have");
        status = build_outcome(allocator, owned, &problem_view, &decision,
                               output, error);
        goto cleanup;
    }

    status = lower_side(allocator, problem_view.left_source,
                        problem_view.left_source_size,
                        problem_view.left_function_name,
                        problem_view.left_function_name_size, &left,
                        &left_supported, error);
    if (status == QL_STATUS_OK) {
        status = lower_side(allocator, problem_view.right_source,
                            problem_view.right_source_size,
                            problem_view.right_function_name,
                            problem_view.right_function_name_size, &right,
                            &right_supported, error);
    }
    if (status != QL_STATUS_OK) {
        goto cleanup;
    }
    if (left_supported == 0u || right_supported == 0u) {
        set_diagnostic(&decision,
                       "the semantic C lowering does not support one of the two functions");
        status = build_outcome(allocator, owned, &problem_view, &decision,
                               output, error);
        goto cleanup;
    }

    {
        ql_egraph_config_v1 config;
        ql_egraph_saturation_limits_v1 limits;
        ql_egraph_proof_result_v1 proof;
        ql_egraph_check_report_view_v1 report_view;
        ql_ir_value_id left_parameters[QL_EGRAPH_PROOF_MAX_PARAMETERS];
        ql_ir_value_id right_parameters[QL_EGRAPH_PROOF_MAX_PARAMETERS];
        size_t left_parameter_count = 0u;
        size_t right_parameter_count = 0u;
        ql_ir_value_id left_return = QL_IR_INVALID_VALUE_ID;
        ql_ir_value_id right_return = QL_IR_INVALID_VALUE_ID;
        term_builder left_builder;
        term_builder right_builder;
        ql_egraph_term_id left_root;
        ql_egraph_term_id right_root;
        uint32_t roots_equal = 0u;
        size_t index;

        ql_egraph_config_init(&config);
        config.max_terms = owned->node_limit;
        config.max_classes = owned->node_limit;
        ql_egraph_saturation_limits_init(&limits);
        limits.max_iterations = owned->iteration_limit;
        limits.max_rewrite_applications = owned->rewrite_limit;

        status = check_shape(&left, config.max_bit_width, &decision,
                             &left_return, error);
        if (status == QL_STATUS_OK) {
            status = check_shape(&right, config.max_bit_width, &decision,
                                 &right_return, error);
        }
        if (status == QL_STATUS_TYPE_MISMATCH) {
            status = build_outcome(allocator, owned, &problem_view, &decision,
                                   output, error);
            goto cleanup;
        }
        if (status != QL_STATUS_OK) {
            goto cleanup;
        }

        status = collect_parameters(&left, left_parameters,
                                    QL_EGRAPH_PROOF_MAX_PARAMETERS,
                                    &left_parameter_count, error);
        if (status == QL_STATUS_OK) {
            status = collect_parameters(&right, right_parameters,
                                        QL_EGRAPH_PROOF_MAX_PARAMETERS,
                                        &right_parameter_count, error);
        }
        if (status == QL_STATUS_TYPE_MISMATCH) {
            set_diagnostic(&decision, error->message);
            status = build_outcome(allocator, owned, &problem_view, &decision,
                                   output, error);
            goto cleanup;
        }
        if (status != QL_STATUS_OK) {
            goto cleanup;
        }
        if (problem_view.argument_binding_count != left_parameter_count ||
            problem_view.argument_binding_count != right_parameter_count) {
            /* The correspondence is total by schema, so a count mismatch
               means the lowered parameter lists disagree with the bound
               signatures; nothing sound can be built on that. */
            set_diagnostic(&decision,
                           "the argument correspondence does not cover the lowered parameter lists");
            status = build_outcome(allocator, owned, &problem_view, &decision,
                                   output, error);
            goto cleanup;
        }

        status = ql_egraph_create(&config, allocator, &graph, error);
        if (status != QL_STATUS_OK) {
            goto cleanup;
        }
        left_terms = allocator->allocate(
            allocator->user_data,
            left.view.value_count * sizeof(*left_terms));
        right_terms = allocator->allocate(
            allocator->user_data,
            right.view.value_count * sizeof(*right_terms));
        if ((left.view.value_count != 0u && left_terms == NULL) ||
            (right.view.value_count != 0u && right_terms == NULL)) {
            ql_error_set(error, QL_STATUS_OUT_OF_MEMORY, NULL);
            status = QL_STATUS_OUT_OF_MEMORY;
            goto cleanup;
        }
        for (index = 0u; index < left.view.value_count; ++index) {
            left_terms[index] = QL_EGRAPH_INVALID_TERM;
        }
        for (index = 0u; index < right.view.value_count; ++index) {
            right_terms[index] = QL_EGRAPH_INVALID_TERM;
        }

        /* One shared variable per argument binding: the left parameter and
           its right counterpart are literally the same term, which is what
           makes cross-side merging possible at all. */
        for (index = 0u; index < problem_view.argument_binding_count;
             ++index) {
            ql_problem_argument_binding_v1 binding;
            ql_ir_type_view_v1 left_type;
            ql_ir_type_view_v1 right_type;
            ql_egraph_term_id variable;
            char symbol[32];
            int written;

            memset(&binding, 0, sizeof(binding));
            binding.struct_size = sizeof(binding);
            status = ql_problem_argument_binding_at(problem, index, &binding,
                                                    error);
            if (status != QL_STATUS_OK) {
                goto cleanup;
            }
            if (binding.left_index >= left_parameter_count ||
                binding.right_index >= right_parameter_count) {
                set_diagnostic(&decision,
                               "an argument binding names a parameter outside the lowered lists");
                status = build_outcome(allocator, owned, &problem_view,
                                       &decision, output, error);
                goto cleanup;
            }
            status = value_type_view(&left,
                                     left_parameters[binding.left_index],
                                     &left_type, error);
            if (status == QL_STATUS_OK) {
                status = value_type_view(&right,
                                         right_parameters[binding.right_index],
                                         &right_type, error);
            }
            if (status != QL_STATUS_OK) {
                goto cleanup;
            }
            if (left_type.kind != right_type.kind ||
                left_type.bit_width != right_type.bit_width ||
                !type_is_graphable(&left_type, config.max_bit_width)) {
                set_diagnostic(&decision,
                               "a bound parameter pair is outside the pure term fragment or type-mismatched");
                status = build_outcome(allocator, owned, &problem_view,
                                       &decision, output, error);
                goto cleanup;
            }
            written = snprintf(symbol, sizeof(symbol), "a%zu", index);
            if (written < 0 || (size_t)written >= sizeof(symbol)) {
                ql_error_set(error, QL_STATUS_INTERNAL_ERROR,
                             "shared variable symbol overflowed");
                status = QL_STATUS_INTERNAL_ERROR;
                goto cleanup;
            }
            if (left_type.kind == QL_IR_TYPE_BOOL) {
                status = ql_egraph_make_bool_variable(graph, symbol,
                                                      &variable, error);
            } else {
                status = ql_egraph_make_bv_variable(
                    graph, symbol, left_type.bit_width, &variable, error);
            }
            if (status != QL_STATUS_OK) {
                goto cleanup;
            }
            left_terms[left_parameters[binding.left_index]] = variable;
            right_terms[right_parameters[binding.right_index]] = variable;
        }

        left_builder.allocator = allocator;
        left_builder.graph = graph;
        left_builder.side = &left;
        left_builder.value_terms = left_terms;
        right_builder.allocator = allocator;
        right_builder.graph = graph;
        right_builder.side = &right;
        right_builder.value_terms = right_terms;

        status = seed_constants(&left_builder, config.max_bit_width, error);
        if (status == QL_STATUS_OK) {
            status = seed_constants(&right_builder, config.max_bit_width,
                                    error);
        }
        if (status == QL_STATUS_OK) {
            status = convert_block(&left_builder, &decision, error);
        }
        if (status == QL_STATUS_OK) {
            status = convert_block(&right_builder, &decision, error);
        }
        if (status == QL_STATUS_TYPE_MISMATCH) {
            status = build_outcome(allocator, owned, &problem_view, &decision,
                                   output, error);
            goto cleanup;
        }
        if (status != QL_STATUS_OK) {
            goto cleanup;
        }
        left_root = left_terms[left_return];
        right_root = right_terms[right_return];
        if (left_root == QL_EGRAPH_INVALID_TERM ||
            right_root == QL_EGRAPH_INVALID_TERM) {
            set_diagnostic(&decision,
                           "a return value never received a term; the pair is outside the fragment");
            status = build_outcome(allocator, owned, &problem_view, &decision,
                                   output, error);
            goto cleanup;
        }

        memset(&proof, 0, sizeof(proof));
        status = ql_egraph_prove_equal(graph, left_root, right_root, &limits,
                                       &proof, error);
        if (status != QL_STATUS_OK) {
            goto cleanup;
        }
        decision.saturation_complete = proof.saturation.complete;
        decision.merge_count = proof.saturation.merge_count;
        decision.term_count = proof.saturation.term_count;
        if (proof.verdict != QL_EGRAPH_VERDICT_PROVED_EQUAL ||
            proof.depends_on_axioms != 0u) {
            set_diagnostic(
                &decision,
                proof.saturation.complete != 0u
                    ? "saturation completed without merging the two return roots; the rewrite set cannot connect them"
                    : "saturation stopped on a resource limit before the roots could merge");
            status = build_outcome(allocator, owned, &problem_view, &decision,
                                   output, error);
            goto cleanup;
        }

        /* The engine says PROVED_EQUAL; the verdict is issued only on the
           independent checker's replay of the same evidence. */
        status = ql_egraph_check_graph(allocator, graph, &report, error);
        if (status != QL_STATUS_OK) {
            goto cleanup;
        }
        memset(&report_view, 0, sizeof(report_view));
        report_view.struct_size = sizeof(report_view);
        status = ql_egraph_check_report_get_view(report, &report_view, error);
        if (status != QL_STATUS_OK) {
            goto cleanup;
        }
        decision.justified_count = report_view.justified_count;
        decision.assumed_count = report_view.assumed_count;
        decision.rejected_count = report_view.rejected_count;
        status = ql_egraph_check_terms_equal(report, left_root, right_root,
                                             &roots_equal, error);
        if (status != QL_STATUS_OK) {
            goto cleanup;
        }
        if (report_view.all_merges_justified == 0u || roots_equal == 0u) {
            /* The engine claimed an equality the replay does not establish.
               That is a defect report, never a verdict. */
            QL_LOGE(QL_EGRAPH_PROOF_CATEGORY,
                    "the replay checker did not confirm the engine's merge "
                    "log (justified %llu, assumed %llu, rejected %llu)",
                    (unsigned long long)report_view.justified_count,
                    (unsigned long long)report_view.assumed_count,
                    (unsigned long long)report_view.rejected_count);
            set_diagnostic(&decision,
                           "the independent replay did not justify the engine's merge log; nothing is claimed");
            status = build_outcome(allocator, owned, &problem_view, &decision,
                                   output, error);
            goto cleanup;
        }

        status = ql_problem_require_proof_binding(problem, error);
        if (status != QL_STATUS_OK) {
            set_diagnostic(&decision, error->message);
            status = build_outcome(allocator, owned, &problem_view, &decision,
                                   output, error);
            goto cleanup;
        }

        decision.verdict =
            proved_verdict_for(problem_view.contract.relation);
        decision.evidence_class = QL_EVIDENCE_PROOF;
        decision.checked_proof = 1u;
        set_diagnostic(&decision,
                       "the replayed merge log establishes return equality on every input of the total fragment");
    }

    status = build_outcome(allocator, owned, &problem_view, &decision, output,
                           error);

cleanup:
    if (left_terms != NULL) {
        allocator->deallocate(allocator->user_data, left_terms);
    }
    if (right_terms != NULL) {
        allocator->deallocate(allocator->user_data, right_terms);
    }
    ql_egraph_check_report_release(report);
    ql_egraph_destroy(graph);
    egraph_proof_side_dispose(&left);
    egraph_proof_side_dispose(&right);
    ql_problem_release(problem);
    if (status == QL_STATUS_OK) {
        ql_error_clear(error);
    }
    return status;
}

static const ql_method_v1 egraph_proof_method = {
    sizeof(ql_method_v1),
    QL_ABI_VERSION,
    QL_EGRAPH_PROOF_METHOD_NAME,
    "Equality-saturation proof with an independently replayed merge log",
    QL_ARTIFACT_KIND_OUTCOME,
    QL_METHOD_DETERMINISTIC | QL_METHOD_PROOF_PRODUCER | QL_METHOD_CACHEABLE,
    1u,
    1u,
    egraph_proof_create,
    egraph_proof_validate,
    egraph_proof_run,
    egraph_proof_destroy,
    { NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL }
};

static const ql_proof_method_v1 egraph_proof_descriptor = {
    sizeof(ql_proof_method_v1),
    QL_ABI_VERSION,
    QL_PROOF_METHOD_FAMILY_REWRITE_EGRAPH,
    &egraph_proof_method,
    egraph_proof_capability,
    { NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL }
};

const ql_method_v1 *QL_CALL ql_egraph_proof_method_entry(void) {
    return &egraph_proof_method;
}

const ql_proof_method_v1 *QL_CALL ql_egraph_proof_method(void) {
    return &egraph_proof_descriptor;
}

ql_status QL_CALL ql_register_egraph_proof_method(ql_registry *registry,
                                                  ql_error *error) {
    ql_status status =
        ql_registry_register(registry, &egraph_proof_method, error);
    if (status != QL_STATUS_OK) {
        return status;
    }
    return ql_registry_register_proof_method(registry,
                                             &egraph_proof_descriptor, error);
}

/* --- Outcome reading ------------------------------------------------------ */

static int read_digest_field(yyjson_val *object, const char *key,
                             ql_digest *digest) {
    yyjson_val *value = yyjson_obj_get(object, key);
    const char *text;
    size_t index;

    memset(digest, 0, sizeof(*digest));
    if (!yyjson_is_str(value) ||
        yyjson_get_len(value) != QL_DIGEST_HEX_SIZE - 1u) {
        return 0;
    }
    text = yyjson_get_str(value);
    for (index = 0u; index < QL_DIGEST_SIZE; ++index) {
        uint32_t byte = 0u;
        uint32_t nibble;
        for (nibble = 0u; nibble < 2u; ++nibble) {
            const char ch = text[index * 2u + nibble];
            uint32_t digit;
            if (ch >= '0' && ch <= '9') {
                digit = (uint32_t)(ch - '0');
            } else if (ch >= 'a' && ch <= 'f') {
                digit = (uint32_t)(ch - 'a' + 10);
            } else {
                return 0;
            }
            byte = (byte << 4) | digit;
        }
        digest->bytes[index] = (uint8_t)byte;
    }
    return 1;
}

static ql_verdict verdict_parse(const char *text) {
    ql_verdict verdict;
    for (verdict = QL_VERDICT_UNKNOWN; verdict <= QL_VERDICT_BOUNDED_CLEAN;
         ++verdict) {
        if (strcmp(ql_verdict_string(verdict), text) == 0) {
            return verdict;
        }
    }
    return QL_VERDICT_UNKNOWN;
}

static ql_evidence_class evidence_parse(const char *text) {
    ql_evidence_class evidence;
    for (evidence = QL_EVIDENCE_PROOF; evidence <= QL_EVIDENCE_UNKNOWN;
         ++evidence) {
        if (strcmp(ql_evidence_class_string(evidence), text) == 0) {
            return evidence;
        }
    }
    return QL_EVIDENCE_UNKNOWN;
}

ql_status QL_CALL ql_egraph_proof_outcome_read(
    const ql_artifact *artifact, ql_egraph_proof_outcome_view_v1 *view,
    ql_error *error) {
    ql_artifact_view artifact_view;
    yyjson_doc *document = NULL;
    yyjson_read_err read_error;
    yyjson_val *root;
    yyjson_val *saturation_object;
    yyjson_val *replay_object;
    yyjson_val *trust;
    const char *text;
    ql_status status = QL_STATUS_OK;

    if (artifact == NULL || view == NULL) {
        ql_error_set(error, QL_STATUS_INVALID_ARGUMENT,
                     "outcome artifact and view are required");
        return QL_STATUS_INVALID_ARGUMENT;
    }
    if (view->struct_size != 0u && view->struct_size < sizeof(*view)) {
        ql_error_set(error, QL_STATUS_ABI_MISMATCH,
                     "outcome view structure is too small");
        return QL_STATUS_ABI_MISMATCH;
    }
    memset(&artifact_view, 0, sizeof(artifact_view));
    artifact_view.struct_size = sizeof(artifact_view);
    status = ql_artifact_get_view(artifact, &artifact_view, error);
    if (status != QL_STATUS_OK) {
        return status;
    }
    if (strcmp(artifact_view.kind, QL_ARTIFACT_KIND_OUTCOME) != 0 ||
        artifact_view.schema_version !=
            QL_EGRAPH_PROOF_OUTCOME_SCHEMA_VERSION) {
        ql_error_set(error, QL_STATUS_TYPE_MISMATCH,
                     "artifact is not a schema v1 quodlibet.outcome");
        return QL_STATUS_TYPE_MISMATCH;
    }
    document = yyjson_read_opts((char *)(uintptr_t)artifact_view.data,
                                artifact_view.size, 0u, NULL, &read_error);
    if (document == NULL) {
        ql_error_set(error, QL_STATUS_PARSE_ERROR,
                     "invalid outcome JSON at byte %zu: %s", read_error.pos,
                     read_error.msg != NULL ? read_error.msg : "parse error");
        return QL_STATUS_PARSE_ERROR;
    }
    memset(view, 0, sizeof(*view));
    view->struct_size = sizeof(*view);
    view->schema_version = QL_EGRAPH_PROOF_OUTCOME_SCHEMA_VERSION;
    root = yyjson_doc_get_root(document);
    saturation_object = yyjson_obj_get(root, "saturation");
    replay_object = yyjson_obj_get(root, "replay");
    trust = yyjson_obj_get(root, "trust");
    if (!yyjson_is_obj(root) || !yyjson_is_obj(saturation_object) ||
        !yyjson_is_obj(replay_object) || !yyjson_is_obj(trust)) {
        ql_error_set(error, QL_STATUS_SCHEMA_MISMATCH,
                     "outcome JSON does not match schema version 1");
        status = QL_STATUS_SCHEMA_MISMATCH;
        goto cleanup;
    }
    text = yyjson_get_str(yyjson_obj_get(root, "method"));
    if (text == NULL || strcmp(text, QL_EGRAPH_PROOF_METHOD_NAME) != 0) {
        ql_error_set(error, QL_STATUS_TYPE_MISMATCH,
                     "outcome was not produced by %s",
                     QL_EGRAPH_PROOF_METHOD_NAME);
        status = QL_STATUS_TYPE_MISMATCH;
        goto cleanup;
    }
    text = yyjson_get_str(yyjson_obj_get(root, "verdict"));
    view->verdict = text != NULL ? verdict_parse(text) : QL_VERDICT_UNKNOWN;
    text = yyjson_get_str(yyjson_obj_get(root, "evidence_class"));
    view->evidence_class =
        text != NULL ? evidence_parse(text) : QL_EVIDENCE_UNKNOWN;
    view->checked_proof =
        yyjson_get_bool(yyjson_obj_get(trust, "checked_proof")) ? 1u : 0u;
    view->saturation_complete =
        yyjson_get_bool(yyjson_obj_get(saturation_object, "complete")) ? 1u
                                                                       : 0u;
    view->term_count =
        yyjson_get_uint(yyjson_obj_get(saturation_object, "terms"));
    view->merge_count =
        yyjson_get_uint(yyjson_obj_get(saturation_object, "merges"));
    view->justified_count =
        yyjson_get_uint(yyjson_obj_get(replay_object, "justified"));
    view->assumed_count =
        yyjson_get_uint(yyjson_obj_get(replay_object, "assumed"));
    view->rejected_count =
        yyjson_get_uint(yyjson_obj_get(replay_object, "rejected"));
    view->rule_catalogue_version = (uint32_t)yyjson_get_uint(
        yyjson_obj_get(replay_object, "rule_catalogue_version"));
    (void)read_digest_field(replay_object, "rule_catalogue_digest",
                            &view->rule_catalogue_digest);
    (void)read_digest_field(root, "problem_digest", &view->problem_digest);
    (void)read_digest_field(root, "cache_key", &view->cache_key);
    text = yyjson_get_str(yyjson_obj_get(root, "diagnostic"));
    if (text != NULL) {
        size_t length = strlen(text);
        if (length >= sizeof(view->diagnostic)) {
            length = sizeof(view->diagnostic) - 1u;
        }
        memcpy(view->diagnostic, text, length);
        view->diagnostic[length] = '\0';
    }
    ql_error_clear(error);

cleanup:
    yyjson_doc_free(document);
    return status;
}
