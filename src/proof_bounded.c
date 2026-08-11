#include "quodlibet/proof_bounded.h"

#include <stdio.h>
#include <string.h>

#include "quodlibet/c_lower.h"
#include "quodlibet/log.h"
#include "quodlibet/problem.h"
#include "quodlibet/product.h"
#include "quodlibet/replay.h"
#include "quodlibet/solver.h"

#include "unroll.h"
#include "yyjson.h"

#define QL_BOUNDED_CATEGORY "proof.bounded-symbolic"

typedef struct bounded_instance {
    ql_allocator allocator;
    uint64_t unroll_bound;
    uint64_t timeout_ms;
    uint64_t memory_limit_mb;
    char *solver_options;
} bounded_instance;

typedef struct bounded_decision {
    ql_verdict verdict;
    ql_evidence_class evidence_class;
    uint32_t replay_confirmed;
    uint32_t bound_cut_used;
    uint64_t left_blocks;
    uint64_t right_blocks;
    const char *backend_name;
    const char *backend_version;
    ql_digest backend_binary_digest;
    ql_digest solver_query_digest;
    char diagnostic[QL_ERROR_MESSAGE_CAPACITY];
} bounded_decision;

/* --- yyjson glue ---------------------------------------------------------- */

static void *bounded_json_allocate(void *context, size_t size) {
    ql_allocator *allocator = (ql_allocator *)context;
    return allocator->allocate(allocator->user_data, size);
}

static void *bounded_json_reallocate(void *context, void *pointer,
                                     size_t old_size, size_t size) {
    ql_allocator *allocator = (ql_allocator *)context;
    (void)old_size;
    return allocator->reallocate(allocator->user_data, pointer, size);
}

static void bounded_json_deallocate(void *context, void *pointer) {
    ql_allocator *allocator = (ql_allocator *)context;
    allocator->deallocate(allocator->user_data, pointer);
}

static yyjson_alc bounded_json_allocator(ql_allocator *allocator) {
    yyjson_alc result;
    result.malloc = bounded_json_allocate;
    result.realloc = bounded_json_reallocate;
    result.free = bounded_json_deallocate;
    result.ctx = allocator;
    return result;
}

static void set_diagnostic(bounded_decision *decision, const char *message) {
    size_t length = strlen(message);
    if (length >= sizeof(decision->diagnostic)) {
        length = sizeof(decision->diagnostic) - 1u;
    }
    memcpy(decision->diagnostic, message, length);
    decision->diagnostic[length] = '\0';
}

/* --- Options -------------------------------------------------------------- */

static ql_status parse_options(bounded_instance *instance,
                               const char *options_json, ql_error *error) {
    static const char *const known[] = {"unroll_bound", "timeout_ms",
                                        "memory_limit_mb", "solver_options"};
    yyjson_alc json_allocator = bounded_json_allocator(&instance->allocator);
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
                     QL_BOUNDED_METHOD_NAME, read_error.pos,
                     read_error.msg != NULL ? read_error.msg : "parse error");
        return QL_STATUS_PARSE_ERROR;
    }
    root = yyjson_doc_get_root(document);
    if (!yyjson_is_obj(root)) {
        ql_error_set(error, QL_STATUS_PARSE_ERROR,
                     "%s options must be a JSON object",
                     QL_BOUNDED_METHOD_NAME);
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
                         QL_BOUNDED_METHOD_NAME, name != NULL ? name : "");
            status = QL_STATUS_INVALID_ARGUMENT;
            goto cleanup;
        }
    }

    value = yyjson_obj_get(root, "unroll_bound");
    if (value != NULL) {
        if (!yyjson_is_uint(value) || yyjson_get_uint(value) == 0u ||
            yyjson_get_uint(value) > QL_BOUNDED_MAX_UNROLL_BOUND) {
            ql_error_set(error, QL_STATUS_INVALID_ARGUMENT,
                         "unroll_bound must be between 1 and %llu",
                         (unsigned long long)QL_BOUNDED_MAX_UNROLL_BOUND);
            status = QL_STATUS_INVALID_ARGUMENT;
            goto cleanup;
        }
        instance->unroll_bound = yyjson_get_uint(value);
    }
    value = yyjson_obj_get(root, "timeout_ms");
    if (value != NULL) {
        if (!yyjson_is_uint(value)) {
            ql_error_set(error, QL_STATUS_INVALID_ARGUMENT,
                         "timeout_ms must be a non-negative integer");
            status = QL_STATUS_INVALID_ARGUMENT;
            goto cleanup;
        }
        instance->timeout_ms = yyjson_get_uint(value);
    }
    value = yyjson_obj_get(root, "memory_limit_mb");
    if (value != NULL) {
        if (!yyjson_is_uint(value)) {
            ql_error_set(error, QL_STATUS_INVALID_ARGUMENT,
                         "memory_limit_mb must be a non-negative integer");
            status = QL_STATUS_INVALID_ARGUMENT;
            goto cleanup;
        }
        instance->memory_limit_mb = yyjson_get_uint(value);
    }
    value = yyjson_obj_get(root, "solver_options");
    if (value != NULL) {
        const char *text;
        size_t length;
        if (!yyjson_is_str(value)) {
            ql_error_set(error, QL_STATUS_INVALID_ARGUMENT,
                         "solver_options must be a JSON string");
            status = QL_STATUS_INVALID_ARGUMENT;
            goto cleanup;
        }
        text = yyjson_get_str(value);
        length = yyjson_get_len(value);
        instance->solver_options = instance->allocator.allocate(
            instance->allocator.user_data, length + 1u);
        if (instance->solver_options == NULL) {
            ql_error_set(error, QL_STATUS_OUT_OF_MEMORY, NULL);
            status = QL_STATUS_OUT_OF_MEMORY;
            goto cleanup;
        }
        memcpy(instance->solver_options, text, length);
        instance->solver_options[length] = '\0';
    }

cleanup:
    yyjson_doc_free(document);
    return status;
}

/* --- Capability ----------------------------------------------------------- */

static ql_status QL_CALL bounded_capability(
    const char *options_json, ql_proof_method_capability_v1 *capability,
    ql_error *error) {
    bounded_instance probe;
    ql_status status;

    memset(&probe, 0, sizeof(probe));
    probe.allocator = *ql_default_allocator();
    probe.unroll_bound = QL_BOUNDED_DEFAULT_UNROLL_BOUND;
    probe.timeout_ms = QL_BOUNDED_DEFAULT_TIMEOUT_MS;
    status = parse_options(&probe, options_json, error);
    if (probe.solver_options != NULL) {
        probe.allocator.deallocate(probe.allocator.user_data,
                                   probe.solver_options);
    }
    if (status != QL_STATUS_OK) {
        return status;
    }
    capability->family = QL_PROOF_METHOD_FAMILY_BOUNDED_EXECUTION;
    /* Never QL_PROOF_SOUNDNESS_PROOF under any option: the unrolled program
       is an under-approximation, so an UNSAT here bounds and proves
       nothing. */
    capability->soundness_classes =
        QL_PROOF_SOUNDNESS_COUNTEREXAMPLE | QL_PROOF_SOUNDNESS_BOUNDED;
    capability->result_kinds = QL_PROOF_RESULT_COUNTEREXAMPLE |
                               QL_PROOF_RESULT_BOUNDED |
                               QL_PROOF_RESULT_UNKNOWN;
    capability->supported_relations = QL_PROOF_RELATION_ALL;
    capability->supported_ub_policies = QL_PROOF_UB_ALL;
    capability->supported_observations = QL_OBSERVE_ALL;
    capability->supported_memory_observations =
        QL_PROOF_MEMORY_MODE(QL_MEMORY_IGNORE) |
        QL_PROOF_MEMORY_MODE(QL_MEMORY_FINAL_REACHABLE_STATE);
    capability->supported_external_call_observations =
        QL_PROOF_EXTERNAL_CALL_MODE(QL_EXTERNAL_CALLS_IGNORE) |
        QL_PROOF_EXTERNAL_CALL_MODE(QL_EXTERNAL_CALLS_ORDERED_TRACE);
    capability->flags = QL_PROOF_CAPABILITY_PRECONDITIONS;
    ql_error_clear(error);
    return QL_STATUS_OK;
}

/* --- Lowering and unrolling ----------------------------------------------- */

typedef struct bounded_side {
    ql_c_frontend_unit *unit;
    ql_c_lower_result *result;
    /* The lowered function as the frontend produced it; the replay path runs
       this one, cyclic or not, so a counterexample speaks about the real
       function. */
    ql_ir *ir;
    /* The bounded acyclic rebuild the product encoder consumes. */
    ql_artifact *unrolled_artifact;
    ql_ir *unrolled;
    ql_unroll_stats_v1 stats;
} bounded_side;

static void bounded_side_dispose(bounded_side *side) {
    ql_ir_release(side->unrolled);
    ql_artifact_release(side->unrolled_artifact);
    ql_ir_release(side->ir);
    ql_c_lower_result_destroy(side->result);
    ql_c_frontend_unit_destroy(side->unit);
    memset(side, 0, sizeof(*side));
}

static ql_status lower_side(const ql_allocator *allocator, const char *source,
                            size_t source_size, const char *name,
                            size_t name_size, bounded_side *side,
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
    *supported = 1u;
    return QL_STATUS_OK;
}

/* Unrolls one side. A shape the transform cannot order, or a rebuilt graph
   the IR reader rejects, is a fact about this method's coverage: it is
   reported as TYPE_MISMATCH and becomes an UNKNOWN outcome, never a
   narrower question. */
static ql_status unroll_side(const ql_allocator *allocator, bounded_side *side,
                             uint32_t bound, ql_error *error) {
    ql_status status;

    side->stats.struct_size = sizeof(side->stats);
    status = ql_ir_unroll_bounded(allocator, side->ir, bound, &side->stats,
                                  &side->unrolled_artifact, error);
    if (status != QL_STATUS_OK) {
        return status;
    }
    status = ql_ir_open(allocator, side->unrolled_artifact, &side->unrolled,
                        error);
    if (status != QL_STATUS_OK) {
        /* The reader found a rebuilt graph it cannot admit, typically an SSA
           ordering the copy-based renaming cannot express. */
        QL_LOGW(QL_BOUNDED_CATEGORY,
                "the unrolled graph was rejected by the IR reader: %s",
                error->message);
        ql_error_set(error, QL_STATUS_TYPE_MISMATCH,
                     "the loop shape is outside bounded unrolling: %s",
                     error->message);
        return QL_STATUS_TYPE_MISMATCH;
    }
    return QL_STATUS_OK;
}

/* --- Solver --------------------------------------------------------------- */

/* Mirrors proof_smt.c: the tightest of the method's own timeout and the run
   budget is handed to the solver subprocess as its deadline. */
static uint64_t solver_deadline_ms(uint64_t method_timeout_ms,
                                   const ql_run_context_v1 *context) {
    uint64_t remaining_ns;
    uint64_t remaining_ms;

    if (context == NULL ||
        context->struct_size < offsetof(ql_run_context_v1, reserved) ||
        context->remaining_ns == NULL) {
        return method_timeout_ms;
    }
    remaining_ns = context->remaining_ns(context->cancel_state);
    if (remaining_ns == UINT64_MAX) {
        return method_timeout_ms;
    }
    remaining_ms = remaining_ns == 0u
                       ? 1u
                       : (remaining_ns + UINT64_C(999999)) / UINT64_C(1000000);
    if (method_timeout_ms == 0u || remaining_ms < method_timeout_ms) {
        return remaining_ms;
    }
    return method_timeout_ms;
}

static ql_status run_check(const ql_allocator *allocator,
                           const bounded_instance *instance,
                           const ql_run_context_v1 *context,
                           const ql_product_query_view_v1 *query_view,
                           const ql_artifact *prefix,
                           const ql_artifact *terminal,
                           ql_solver_check_result_v1 *result,
                           ql_error *error) {
    const ql_solver_descriptor_v1 *descriptor =
        ql_bitwuzla_solver_descriptor();
    ql_solver_check_request_v1 request;
    ql_solver *solver = NULL;
    ql_status status;

    if (descriptor->capability.availability == QL_SOLVER_UNAVAILABLE) {
        ql_error_set(error, QL_STATUS_NOT_FOUND,
                     "the canonical Bitwuzla backend is not available in this build");
        return QL_STATUS_NOT_FOUND;
    }
    if (context != NULL &&
        context->struct_size >= offsetof(ql_run_context_v1, reserved) &&
        context->solver_session != NULL) {
        status = ql_solver_create_in_session(
            allocator, (ql_solver_session *)context->solver_session, &solver,
            error);
    } else {
        status = ql_solver_create(allocator, descriptor,
                                  instance->solver_options, &solver, error);
    }
    if (status != QL_STATUS_OK) {
        return status;
    }
    status = ql_solver_add_smt2(solver, prefix, error);
    if (status == QL_STATUS_OK) {
        status = ql_solver_add_smt2(solver, terminal, error);
    }
    if (status != QL_STATUS_OK) {
        ql_solver_destroy(solver);
        return status;
    }
    ql_solver_check_request_init(&request, query_view->logic);
    request.maximum_bv_width = query_view->maximum_bv_width;
    request.timeout_ms = solver_deadline_ms(instance->timeout_ms, context);
    if (instance->memory_limit_mb != 0u) {
        request.memory_limit_mb = instance->memory_limit_mb;
        request.required_features = QL_SOLVER_FEATURE_MEMORY_LIMIT;
    }
    request.artifact_requests = QL_SOLVER_REQUEST_MODEL;
    if (context != NULL && context->is_cancelled != NULL) {
        request.cancel_state = context->cancel_state;
        request.is_cancelled = context->is_cancelled;
    }
    ql_solver_check_result_init(result);
    status = ql_solver_check(solver, &request, result, error);
    ql_solver_destroy(solver);
    return status;
}

/* Decodes and concretely replays one SAT model against the original cyclic
   IRs. Confirmation is written into `decision` and the counterexample
   artifact; a model that does not reproduce leaves both untouched. */
static ql_status replay_model(const ql_allocator *allocator,
                              const ql_problem *problem,
                              const ql_product_query *query,
                              const ql_ir *left_ir, const ql_ir *right_ir,
                              const ql_artifact *model,
                              bounded_decision *decision,
                              ql_artifact **counterexample, ql_error *error) {
    ql_replay_witness *witness = NULL;
    ql_replay_result_v1 replay;
    ql_status status;

    status = ql_replay_decode_model(allocator, query, model, &witness, error);
    if (status != QL_STATUS_OK) {
        QL_LOGE(QL_BOUNDED_CATEGORY, "a solver model could not be decoded: %s",
                error->message);
        ql_error_clear(error);
        return QL_STATUS_OK;
    }
    memset(&replay, 0, sizeof(replay));
    replay.struct_size = sizeof(replay);
    status = ql_replay_execute(allocator, problem, query, left_ir, right_ir,
                               witness, &replay, error);
    if (status != QL_STATUS_OK) {
        ql_replay_witness_destroy(witness);
        return status;
    }
    if (replay.conclusive == 0u || replay.violated == 0u) {
        ql_replay_witness_destroy(witness);
        return QL_STATUS_OK;
    }
    status = ql_replay_counterexample_artifact_create(
        allocator, query, witness, &replay, counterexample, error);
    ql_replay_witness_destroy(witness);
    if (status != QL_STATUS_OK) {
        return status;
    }
    decision->verdict = QL_VERDICT_COUNTEREXAMPLE;
    decision->evidence_class = QL_EVIDENCE_COUNTEREXAMPLE;
    decision->replay_confirmed = 1u;
    set_diagnostic(decision,
                   "a decoded solver model reproduced the violation when both "
                   "original functions were executed concretely");
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
                                   const bounded_instance *instance,
                                   const ql_product_query_view_v1 *query,
                                   const ql_digest *backend_binary,
                                   ql_digest *cache_key, ql_error *error) {
    /* The unroll bound changes the question, the query digests change with
       the encoding, and the backend answers it, so all three are identity. */
    char identity[640];
    char prefix_hex[QL_DIGEST_HEX_SIZE];
    char violation_hex[QL_DIGEST_HEX_SIZE];
    char backend_hex[QL_DIGEST_HEX_SIZE];
    ql_cache_key_input_v1 input;
    int written;

    ql_digest_hex(&query->prefix_digest, prefix_hex);
    ql_digest_hex(&query->violation_digest, violation_hex);
    ql_digest_hex(backend_binary, backend_hex);
    written = snprintf(
        identity, sizeof(identity),
        "{\"unroll_bound\":%llu,\"backend\":\"%s\",\"prefix\":\"%s\",\"violation\":\"%s\"}",
        (unsigned long long)instance->unroll_bound, backend_hex, prefix_hex,
        violation_hex);
    if (written < 0 || (size_t)written >= sizeof(identity)) {
        ql_error_set(error, QL_STATUS_INTERNAL_ERROR,
                     "could not format the cache identity");
        return QL_STATUS_INTERNAL_ERROR;
    }
    memset(&input, 0, sizeof(input));
    input.struct_size = sizeof(input);
    input.artifact_digest = *problem_digest;
    input.semantic_problem_digest = *problem_digest;
    input.method_name = QL_BOUNDED_METHOD_NAME;
    input.method_version = QL_BOUNDED_METHOD_VERSION;
    input.canonical_options = identity;
    input.canonical_options_size = (size_t)written;
    return ql_cache_key_compute(&input, cache_key, error);
}

static ql_status build_outcome(const ql_allocator *allocator,
                               const bounded_instance *instance,
                               const ql_problem_view_v2 *problem_view,
                               const ql_product_query_view_v1 *query_view,
                               const bounded_decision *decision,
                               const ql_artifact *counterexample,
                               ql_artifact **output, ql_error *error) {
    ql_allocator allocator_copy = *allocator;
    yyjson_alc json_allocator = bounded_json_allocator(&allocator_copy);
    yyjson_mut_doc *document = NULL;
    yyjson_mut_val *root;
    yyjson_mut_val *bound_object;
    yyjson_mut_val *solver_object;
    yyjson_mut_val *trust_object;
    yyjson_write_err write_error;
    ql_artifact_view counterexample_view;
    ql_digest cache_key;
    ql_digest empty_digest;
    char *json = NULL;
    size_t json_size = 0u;
    ql_status status;

    memset(&empty_digest, 0, sizeof(empty_digest));
    memset(&counterexample_view, 0, sizeof(counterexample_view));
    counterexample_view.struct_size = sizeof(counterexample_view);
    if (counterexample != NULL) {
        status = ql_artifact_get_view(counterexample, &counterexample_view,
                                      error);
        if (status != QL_STATUS_OK) {
            return status;
        }
    }
    status = compute_cache_key(&problem_view->artifact_digest, instance,
                               query_view, &decision->backend_binary_digest,
                               &cache_key, error);
    if (status != QL_STATUS_OK) {
        return status;
    }

    document = yyjson_mut_doc_new(&json_allocator);
    root = document != NULL ? yyjson_mut_obj(document) : NULL;
    bound_object = document != NULL ? yyjson_mut_obj(document) : NULL;
    solver_object = document != NULL ? yyjson_mut_obj(document) : NULL;
    trust_object = document != NULL ? yyjson_mut_obj(document) : NULL;
    if (document == NULL || root == NULL || bound_object == NULL ||
        solver_object == NULL || trust_object == NULL ||
        !yyjson_mut_obj_add_str(document, root, "kind",
                                QL_ARTIFACT_KIND_OUTCOME) ||
        !yyjson_mut_obj_add_uint(document, root, "schema_version",
                                 QL_BOUNDED_OUTCOME_SCHEMA_VERSION) ||
        !yyjson_mut_obj_add_str(document, root, "method",
                                QL_BOUNDED_METHOD_NAME) ||
        !yyjson_mut_obj_add_str(document, root, "method_version",
                                QL_BOUNDED_METHOD_VERSION) ||
        !yyjson_mut_obj_add_str(document, root, "verdict",
                                ql_verdict_string(decision->verdict)) ||
        !yyjson_mut_obj_add_str(
            document, root, "evidence_class",
            ql_evidence_class_string(decision->evidence_class)) ||
        !add_digest(document, root, "problem_digest",
                    &problem_view->artifact_digest) ||
        !add_digest(document, root, "cache_key", &cache_key) ||
        !yyjson_mut_obj_add_uint(document, root, "relation",
                                 (uint64_t)query_view->relation) ||
        !yyjson_mut_obj_add_uint(document, root, "ub_policy",
                                 (uint64_t)query_view->ub_policy) ||
        !yyjson_mut_obj_add_uint(document, root, "observations",
                                 query_view->covered_observations) ||
        !yyjson_mut_obj_add_strcpy(document, root, "diagnostic",
                                   decision->diagnostic)) {
        status = QL_STATUS_OUT_OF_MEMORY;
        ql_error_set(error, status, NULL);
        goto cleanup;
    }
    /* The bound vector: what was searched, in the dimension the search is
       bounded in. BOUNDED_CLEAN is meaningless without it. */
    if (!yyjson_mut_obj_add_uint(document, bound_object, "unroll_bound",
                                 instance->unroll_bound) ||
        !yyjson_mut_obj_add_bool(document, bound_object, "bound_cut_used",
                                 decision->bound_cut_used != 0u) ||
        !yyjson_mut_obj_add_uint(document, bound_object,
                                 "left_blocks_emitted",
                                 decision->left_blocks) ||
        !yyjson_mut_obj_add_uint(document, bound_object,
                                 "right_blocks_emitted",
                                 decision->right_blocks) ||
        !yyjson_mut_obj_add_val(document, root, "bound", bound_object)) {
        status = QL_STATUS_OUT_OF_MEMORY;
        ql_error_set(error, status, NULL);
        goto cleanup;
    }
    if (!yyjson_mut_obj_add_str(document, solver_object, "backend",
                                decision->backend_name != NULL
                                    ? decision->backend_name
                                    : "") ||
        !yyjson_mut_obj_add_str(document, solver_object, "backend_version",
                                decision->backend_version != NULL
                                    ? decision->backend_version
                                    : "") ||
        !add_digest(document, solver_object, "backend_binary_digest",
                    &decision->backend_binary_digest) ||
        !add_digest(document, solver_object, "query_digest",
                    &decision->solver_query_digest) ||
        !add_digest(document, solver_object, "prefix_digest",
                    &query_view->prefix_digest) ||
        !add_digest(document, solver_object, "violation_digest",
                    &query_view->violation_digest) ||
        !yyjson_mut_obj_add_val(document, root, "solver", solver_object)) {
        status = QL_STATUS_OUT_OF_MEMORY;
        ql_error_set(error, status, NULL);
        goto cleanup;
    }
    if (!yyjson_mut_obj_add_bool(document, trust_object, "checked_proof", 0) ||
        !yyjson_mut_obj_add_bool(document, trust_object, "replay_confirmed",
                                 decision->replay_confirmed != 0u) ||
        !yyjson_mut_obj_add_str(
            document, trust_object, "basis",
            decision->verdict == QL_VERDICT_COUNTEREXAMPLE
                ? "both original functions were executed concretely on the decoded input and the declared relation did not hold"
                : decision->verdict == QL_VERDICT_BOUNDED_CLEAN
                      ? "no violating input exists whose execution stays within the recorded unroll bound; nothing is claimed beyond it"
                      : "no sound conclusion was reached") ||
        !yyjson_mut_obj_add_val(document, root, "trust", trust_object)) {
        status = QL_STATUS_OUT_OF_MEMORY;
        ql_error_set(error, status, NULL);
        goto cleanup;
    }
    if (counterexample != NULL) {
        if (!add_digest(document, root, "counterexample_digest",
                        &counterexample_view.digest) ||
            !yyjson_mut_obj_add_strncpy(document, root, "counterexample",
                                        (const char *)counterexample_view.data,
                                        counterexample_view.size)) {
            status = QL_STATUS_OUT_OF_MEMORY;
            ql_error_set(error, status, NULL);
            goto cleanup;
        }
    } else if (!add_digest(document, root, "counterexample_digest",
                           &empty_digest) ||
               !yyjson_mut_obj_add_null(document, root, "counterexample")) {
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
                                QL_BOUNDED_OUTCOME_SCHEMA_VERSION, json,
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

static ql_status QL_CALL bounded_create(const ql_host_v1 *host,
                                        const char *options_json,
                                        void **instance, ql_error *error) {
    bounded_instance *created;
    ql_allocator allocator;
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
    created->unroll_bound = QL_BOUNDED_DEFAULT_UNROLL_BOUND;
    created->timeout_ms = QL_BOUNDED_DEFAULT_TIMEOUT_MS;
    status = parse_options(created, options_json, error);
    if (status != QL_STATUS_OK) {
        if (created->solver_options != NULL) {
            allocator.deallocate(allocator.user_data,
                                 created->solver_options);
        }
        allocator.deallocate(allocator.user_data, created);
        return status;
    }
    *instance = created;
    ql_error_clear(error);
    return QL_STATUS_OK;
}

static void QL_CALL bounded_destroy(void *instance) {
    bounded_instance *owned = (bounded_instance *)instance;
    ql_allocator allocator;

    if (owned == NULL) {
        return;
    }
    allocator = owned->allocator;
    if (owned->solver_options != NULL) {
        allocator.deallocate(allocator.user_data, owned->solver_options);
    }
    allocator.deallocate(allocator.user_data, owned);
}

static ql_status QL_CALL bounded_validate(void *instance,
                                          ql_artifact *const *inputs,
                                          size_t input_count,
                                          ql_error *error) {
    ql_artifact_view view;
    ql_status status;

    (void)instance;
    if (inputs == NULL || input_count != 1u || inputs[0] == NULL) {
        ql_error_set(error, QL_STATUS_INVALID_ARGUMENT,
                     "%s consumes exactly one quodlibet.problem artifact",
                     QL_BOUNDED_METHOD_NAME);
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
                     QL_BOUNDED_METHOD_NAME);
        return QL_STATUS_TYPE_MISMATCH;
    }
    if (view.schema_version != QL_PROBLEM_SCHEMA_VERSION_2) {
        ql_error_set(error, QL_STATUS_TYPE_MISMATCH,
                     "%s requires problem schema v2; schema v%u records no argument correspondence",
                     QL_BOUNDED_METHOD_NAME, view.schema_version);
        return QL_STATUS_TYPE_MISMATCH;
    }
    ql_error_clear(error);
    return QL_STATUS_OK;
}

static ql_status QL_CALL bounded_run(void *instance,
                                     const ql_run_context_v1 *context,
                                     ql_artifact *const *inputs,
                                     size_t input_count, ql_artifact **output,
                                     ql_error *error) {
    bounded_instance *owned = (bounded_instance *)instance;
    const ql_allocator *allocator;
    ql_problem *problem = NULL;
    ql_problem_view_v2 problem_view;
    ql_product_query *query = NULL;
    ql_product_query_view_v1 query_view;
    ql_solver_check_result_v1 violation;
    ql_artifact *counterexample = NULL;
    bounded_side left;
    bounded_side right;
    bounded_decision decision;
    uint32_t left_supported = 0u;
    uint32_t right_supported = 0u;
    ql_status status;

    memset(&left, 0, sizeof(left));
    memset(&right, 0, sizeof(right));
    memset(&decision, 0, sizeof(decision));
    ql_solver_check_result_init(&violation);
    decision.verdict = QL_VERDICT_UNKNOWN;
    decision.evidence_class = QL_EVIDENCE_UNKNOWN;
    if (owned == NULL || context == NULL || output == NULL) {
        ql_error_set(error, QL_STATUS_INVALID_ARGUMENT,
                     "instance, run context, and output are required");
        return QL_STATUS_INVALID_ARGUMENT;
    }
    *output = NULL;
    status = bounded_validate(instance, inputs, input_count, error);
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
    memset(&query_view, 0, sizeof(query_view));
    query_view.struct_size = sizeof(query_view);
    query_view.relation = problem_view.contract.relation;
    query_view.ub_policy = problem_view.contract.ub_policy;
    query_view.covered_observations = problem_view.contract.observations;

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
        status = build_outcome(allocator, owned, &problem_view, &query_view,
                               &decision, NULL, output, error);
        goto cleanup;
    }

    status = unroll_side(allocator, &left, (uint32_t)owned->unroll_bound,
                         error);
    if (status == QL_STATUS_OK) {
        status = unroll_side(allocator, &right, (uint32_t)owned->unroll_bound,
                             error);
    }
    if (status == QL_STATUS_TYPE_MISMATCH) {
        set_diagnostic(&decision, error->message);
        status = build_outcome(allocator, owned, &problem_view, &query_view,
                               &decision, NULL, output, error);
        goto cleanup;
    }
    if (status != QL_STATUS_OK) {
        goto cleanup;
    }
    decision.bound_cut_used =
        left.stats.bound_cut_used | right.stats.bound_cut_used;
    decision.left_blocks = left.stats.blocks_emitted;
    decision.right_blocks = right.stats.blocks_emitted;

    status = ql_product_query_build(allocator, problem, left.unrolled,
                                    right.unrolled, &query, error);
    if (status == QL_STATUS_TYPE_MISMATCH) {
        set_diagnostic(&decision, error->message);
        status = build_outcome(allocator, owned, &problem_view, &query_view,
                               &decision, NULL, output, error);
        goto cleanup;
    }
    if (status != QL_STATUS_OK) {
        goto cleanup;
    }
    status = ql_product_query_get_view(query, &query_view, error);
    if (status != QL_STATUS_OK) {
        goto cleanup;
    }

    status = run_check(allocator, owned, context, &query_view,
                       ql_product_query_prefix_artifact(query),
                       ql_product_query_violation_artifact(query), &violation,
                       error);
    if (status != QL_STATUS_OK) {
        goto cleanup;
    }
    decision.backend_name = violation.backend_name;
    decision.backend_version = violation.backend_version;
    decision.backend_binary_digest = violation.backend_binary_digest;
    decision.solver_query_digest = violation.query_digest;

    if (violation.kind == QL_SOLVER_CHECK_SAT) {
        if (violation.model_artifact != NULL) {
            status = replay_model(allocator, problem, query, left.ir,
                                  right.ir, violation.model_artifact,
                                  &decision, &counterexample, error);
            if (status != QL_STATUS_OK) {
                goto cleanup;
            }
        }
        if (decision.replay_confirmed == 0u &&
            ql_product_query_bounded_violation_artifact(query) != NULL) {
            /* The unbounded model may describe objects too large to rebuild
               for replay. The bounded variant of the same violation claim
               yields a replayable model; its SAT is still a genuine
               violation, and its UNSAT is not consulted. */
            ql_solver_check_result_v1 bounded;
            ql_solver_check_result_init(&bounded);
            status = run_check(
                allocator, owned, context, &query_view,
                ql_product_query_prefix_artifact(query),
                ql_product_query_bounded_violation_artifact(query), &bounded,
                error);
            if (status == QL_STATUS_OK &&
                bounded.kind == QL_SOLVER_CHECK_SAT &&
                bounded.model_artifact != NULL) {
                status = replay_model(allocator, problem, query, left.ir,
                                      right.ir, bounded.model_artifact,
                                      &decision, &counterexample, error);
            }
            ql_solver_check_result_clear(&bounded);
            if (status != QL_STATUS_OK) {
                goto cleanup;
            }
        }
        if (decision.replay_confirmed == 0u) {
            set_diagnostic(&decision,
                           "the solver reported a violation of the bounded encoding but no model reproduced it concretely");
        }
    } else if (violation.kind == QL_SOLVER_CHECK_UNSAT) {
        decision.verdict = QL_VERDICT_BOUNDED_CLEAN;
        decision.evidence_class = QL_EVIDENCE_BOUNDED;
        set_diagnostic(&decision,
                       decision.bound_cut_used != 0u
                           ? "no violating input stays within the unroll bound; executions beyond it were not examined"
                           : "no violating input exists and neither graph reached the unroll bound");
    } else {
        set_diagnostic(&decision,
                       violation.unknown_reason == QL_SOLVER_UNKNOWN_TIMEOUT
                           ? "the backend timed out on the bounded violation query"
                           : violation.unknown_reason ==
                                     QL_SOLVER_UNKNOWN_CANCELLED
                                 ? "the bounded violation query was cancelled"
                                 : "the backend did not decide the bounded violation query");
    }

    status = build_outcome(allocator, owned, &problem_view, &query_view,
                           &decision, counterexample, output, error);

cleanup:
    ql_solver_check_result_clear(&violation);
    ql_artifact_release(counterexample);
    ql_product_query_destroy(query);
    bounded_side_dispose(&left);
    bounded_side_dispose(&right);
    ql_problem_release(problem);
    if (status == QL_STATUS_OK) {
        ql_error_clear(error);
    }
    return status;
}

static const ql_method_v1 bounded_method = {
    sizeof(ql_method_v1),
    QL_ABI_VERSION,
    QL_BOUNDED_METHOD_NAME,
    "Bounded symbolic search over unrolled cyclic C functions",
    QL_ARTIFACT_KIND_OUTCOME,
    QL_METHOD_COUNTEREXAMPLE_PRODUCER | QL_METHOD_CACHEABLE,
    1u,
    1u,
    bounded_create,
    bounded_validate,
    bounded_run,
    bounded_destroy,
    { NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL }
};

static const ql_proof_method_v1 bounded_descriptor = {
    sizeof(ql_proof_method_v1),
    QL_ABI_VERSION,
    QL_PROOF_METHOD_FAMILY_BOUNDED_EXECUTION,
    &bounded_method,
    bounded_capability,
    { NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL }
};

const ql_method_v1 *QL_CALL ql_bounded_method(void) {
    return &bounded_method;
}

const ql_proof_method_v1 *QL_CALL ql_bounded_proof_method(void) {
    return &bounded_descriptor;
}

ql_status QL_CALL ql_register_bounded_method(ql_registry *registry,
                                             ql_error *error) {
    ql_status status = ql_registry_register(registry, &bounded_method, error);
    if (status != QL_STATUS_OK) {
        return status;
    }
    return ql_registry_register_proof_method(registry, &bounded_descriptor,
                                             error);
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

static ql_status open_outcome_document(const ql_artifact *artifact,
                                       ql_artifact_view *view,
                                       yyjson_doc **document,
                                       ql_error *error) {
    yyjson_read_err read_error;
    ql_status status;

    memset(view, 0, sizeof(*view));
    view->struct_size = sizeof(*view);
    status = ql_artifact_get_view(artifact, view, error);
    if (status != QL_STATUS_OK) {
        return status;
    }
    if (strcmp(view->kind, QL_ARTIFACT_KIND_OUTCOME) != 0 ||
        view->schema_version != QL_BOUNDED_OUTCOME_SCHEMA_VERSION) {
        ql_error_set(error, QL_STATUS_TYPE_MISMATCH,
                     "artifact is not a schema v1 quodlibet.outcome");
        return QL_STATUS_TYPE_MISMATCH;
    }
    *document = yyjson_read_opts((char *)(uintptr_t)view->data, view->size, 0u,
                                 NULL, &read_error);
    if (*document == NULL) {
        ql_error_set(error, QL_STATUS_PARSE_ERROR,
                     "invalid outcome JSON at byte %zu: %s", read_error.pos,
                     read_error.msg != NULL ? read_error.msg : "parse error");
        return QL_STATUS_PARSE_ERROR;
    }
    return QL_STATUS_OK;
}

ql_status QL_CALL ql_bounded_outcome_read(const ql_artifact *artifact,
                                          ql_bounded_outcome_view_v1 *view,
                                          ql_error *error) {
    ql_artifact_view artifact_view;
    yyjson_doc *document = NULL;
    yyjson_val *root;
    yyjson_val *bound_object;
    yyjson_val *trust;
    const char *text;
    ql_status status;

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
    status = open_outcome_document(artifact, &artifact_view, &document, error);
    if (status != QL_STATUS_OK) {
        return status;
    }
    memset(view, 0, sizeof(*view));
    view->struct_size = sizeof(*view);
    view->schema_version = QL_BOUNDED_OUTCOME_SCHEMA_VERSION;
    root = yyjson_doc_get_root(document);
    bound_object = yyjson_obj_get(root, "bound");
    trust = yyjson_obj_get(root, "trust");
    if (!yyjson_is_obj(root) || !yyjson_is_obj(bound_object) ||
        !yyjson_is_obj(trust)) {
        ql_error_set(error, QL_STATUS_SCHEMA_MISMATCH,
                     "outcome JSON does not match schema version 1");
        status = QL_STATUS_SCHEMA_MISMATCH;
        goto cleanup;
    }
    text = yyjson_get_str(yyjson_obj_get(root, "method"));
    if (text == NULL || strcmp(text, QL_BOUNDED_METHOD_NAME) != 0) {
        ql_error_set(error, QL_STATUS_TYPE_MISMATCH,
                     "outcome was not produced by %s", QL_BOUNDED_METHOD_NAME);
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
    view->replay_confirmed =
        yyjson_get_bool(yyjson_obj_get(trust, "replay_confirmed")) ? 1u : 0u;
    view->unroll_bound =
        yyjson_get_uint(yyjson_obj_get(bound_object, "unroll_bound"));
    view->bound_cut_used =
        yyjson_get_bool(yyjson_obj_get(bound_object, "bound_cut_used")) ? 1u
                                                                        : 0u;
    view->left_blocks_emitted =
        yyjson_get_uint(yyjson_obj_get(bound_object, "left_blocks_emitted"));
    view->right_blocks_emitted =
        yyjson_get_uint(yyjson_obj_get(bound_object, "right_blocks_emitted"));
    (void)read_digest_field(root, "problem_digest", &view->problem_digest);
    (void)read_digest_field(root, "cache_key", &view->cache_key);
    (void)read_digest_field(root, "counterexample_digest",
                            &view->counterexample_digest);
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

ql_status QL_CALL ql_bounded_outcome_counterexample(
    const ql_allocator *allocator, const ql_artifact *artifact,
    ql_artifact **output, ql_error *error) {
    ql_artifact_view artifact_view;
    yyjson_doc *document = NULL;
    yyjson_val *value;
    ql_status status;

    if (artifact == NULL || output == NULL) {
        ql_error_set(error, QL_STATUS_INVALID_ARGUMENT,
                     "outcome artifact and output are required");
        return QL_STATUS_INVALID_ARGUMENT;
    }
    *output = NULL;
    status = open_outcome_document(artifact, &artifact_view, &document, error);
    if (status != QL_STATUS_OK) {
        return status;
    }
    value = yyjson_obj_get(yyjson_doc_get_root(document), "counterexample");
    if (yyjson_is_str(value)) {
        status = ql_artifact_create(allocator, QL_ARTIFACT_KIND_COUNTEREXAMPLE,
                                    QL_REPLAY_SCHEMA_VERSION,
                                    yyjson_get_str(value),
                                    yyjson_get_len(value), output, error);
    } else {
        ql_error_clear(error);
    }
    yyjson_doc_free(document);
    return status;
}
