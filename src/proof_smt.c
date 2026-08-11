#include "quodlibet/proof_smt.h"

#include <stddef.h>
#include <string.h>

#include "quodlibet/c_lower.h"
#include "quodlibet/log.h"
#include "quodlibet/problem.h"
#include "quodlibet/product.h"
#include "quodlibet/replay.h"

#include "internal.h"
#include "loop_proof.h"
#include "uv.h"
#include "yyjson.h"

/* This translation unit owns the stage-timing storage. Every other includer of
   stage_timer.h sees the extern declaration; see that header for why. */
#define QL_STAGE_TIMER_DEFINE
#include "stage_timer.h"

#define QL_SMT_PRODUCT_CATEGORY "proof.smt-product"

typedef struct smt_product_instance {
    ql_allocator allocator;
    ql_unsat_promotion_policy unsat_promotion;
    uint64_t timeout_ms;
    uint64_t memory_limit_mb;
    char *solver_options;
} smt_product_instance;

/* Everything the run decided, before it is serialized. */
typedef struct smt_product_decision {
    ql_verdict verdict;
    ql_evidence_class evidence_class;
    ql_smt_product_answer violation_answer;
    ql_smt_product_answer domain_answer;
    uint32_t replay_confirmed;
    ql_digest solver_query_digest;
    ql_digest solver_binary_digest;
    const char *backend_name;
    const char *backend_version;
    char diagnostic[QL_ERROR_MESSAGE_CAPACITY];
    ql_loop_proof_stats_v1 loop;
} smt_product_decision;

static void *smt_json_allocate(void *context, size_t size) {
    ql_allocator *allocator = (ql_allocator *)context;
    return allocator->allocate(allocator->user_data, size);
}

static void *smt_json_reallocate(void *context, void *pointer,
                                 size_t old_size, size_t size) {
    ql_allocator *allocator = (ql_allocator *)context;
    (void)old_size;
    return allocator->reallocate(allocator->user_data, pointer, size);
}

static void smt_json_deallocate(void *context, void *pointer) {
    ql_allocator *allocator = (ql_allocator *)context;
    allocator->deallocate(allocator->user_data, pointer);
}

static yyjson_alc smt_json_allocator(ql_allocator *allocator) {
    yyjson_alc result;
    result.malloc = smt_json_allocate;
    result.realloc = smt_json_reallocate;
    result.free = smt_json_deallocate;
    result.ctx = allocator;
    return result;
}

static void set_diagnostic(smt_product_decision *decision,
                           const char *message) {
    size_t length = strlen(message);
    if (length >= sizeof(decision->diagnostic)) {
        length = sizeof(decision->diagnostic) - 1u;
    }
    memcpy(decision->diagnostic, message, length);
    decision->diagnostic[length] = '\0';
}

static const char *answer_string(ql_smt_product_answer answer) {
    switch (answer) {
    case QL_SMT_PRODUCT_ANSWER_SAT:
        return "sat";
    case QL_SMT_PRODUCT_ANSWER_UNSAT:
        return "unsat";
    case QL_SMT_PRODUCT_ANSWER_UNKNOWN:
        return "unknown";
    default:
        return "not-queried";
    }
}

static ql_smt_product_answer answer_parse(const char *text, size_t size) {
    if (size == 3u && memcmp(text, "sat", 3u) == 0) {
        return QL_SMT_PRODUCT_ANSWER_SAT;
    }
    if (size == 5u && memcmp(text, "unsat", 5u) == 0) {
        return QL_SMT_PRODUCT_ANSWER_UNSAT;
    }
    if (size == 7u && memcmp(text, "unknown", 7u) == 0) {
        return QL_SMT_PRODUCT_ANSWER_UNKNOWN;
    }
    return QL_SMT_PRODUCT_ANSWER_NOT_QUERIED;
}

static ql_smt_product_answer answer_from_solver(ql_solver_check_kind kind) {
    switch (kind) {
    case QL_SOLVER_CHECK_SAT:
        return QL_SMT_PRODUCT_ANSWER_SAT;
    case QL_SOLVER_CHECK_UNSAT:
        return QL_SMT_PRODUCT_ANSWER_UNSAT;
    default:
        return QL_SMT_PRODUCT_ANSWER_UNKNOWN;
    }
}

static const char *promotion_string(ql_unsat_promotion_policy policy) {
    return policy == QL_UNSAT_PROMOTION_TRUSTED_BACKEND ? "trusted-backend"
                                                        : "none";
}

static const char *loop_strategy_string(ql_loop_proof_strategy strategy) {
    switch (strategy) {
    case QL_LOOP_PROOF_STRATEGY_STRUCTURAL_INDUCTION:
        return "structural-induction";
    case QL_LOOP_PROOF_STRATEGY_AFFINE_SUMMARY:
        return "affine-summary";
    case QL_LOOP_PROOF_STRATEGY_CHC_PDR_UNAVAILABLE:
        return "chc-pdr-unavailable";
    case QL_LOOP_PROOF_STRATEGY_EXACT_REFLEXIVITY:
        return "exact-reflexivity";
    default:
        return "none";
    }
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

/* --- Options -------------------------------------------------------------- */

static ql_status parse_options(smt_product_instance *instance,
                               const char *options_json, ql_error *error) {
    static const char *const known[] = {"unsat_promotion", "timeout_ms",
                                        "memory_limit_mb", "solver_options"};
    yyjson_alc json_allocator = smt_json_allocator(&instance->allocator);
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
                     QL_SMT_PRODUCT_METHOD_NAME, read_error.pos,
                     read_error.msg != NULL ? read_error.msg : "parse error");
        return QL_STATUS_PARSE_ERROR;
    }
    root = yyjson_doc_get_root(document);
    if (!yyjson_is_obj(root)) {
        ql_error_set(error, QL_STATUS_PARSE_ERROR,
                     "%s options must be a JSON object",
                     QL_SMT_PRODUCT_METHOD_NAME);
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
                         QL_SMT_PRODUCT_METHOD_NAME,
                         name != NULL ? name : "");
            status = QL_STATUS_INVALID_ARGUMENT;
            goto cleanup;
        }
    }

    value = yyjson_obj_get(root, "unsat_promotion");
    if (value != NULL) {
        const char *text = yyjson_get_str(value);
        if (text == NULL) {
            status = QL_STATUS_INVALID_ARGUMENT;
        } else if (strcmp(text, "none") == 0) {
            instance->unsat_promotion = QL_UNSAT_PROMOTION_NONE;
        } else if (strcmp(text, "trusted-backend") == 0) {
            instance->unsat_promotion = QL_UNSAT_PROMOTION_TRUSTED_BACKEND;
        } else {
            status = QL_STATUS_INVALID_ARGUMENT;
        }
        if (status != QL_STATUS_OK) {
            ql_error_set(error, status,
                         "unsat_promotion must be \"none\" or \"trusted-backend\"");
            goto cleanup;
        }
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
        const char *text = yyjson_get_str(value);
        size_t length;
        if (text == NULL) {
            ql_error_set(error, QL_STATUS_INVALID_ARGUMENT,
                         "solver_options must be a JSON string");
            status = QL_STATUS_INVALID_ARGUMENT;
            goto cleanup;
        }
        length = strlen(text);
        instance->solver_options = instance->allocator.allocate(
            instance->allocator.user_data, length + 1u);
        if (instance->solver_options == NULL) {
            ql_error_set(error, QL_STATUS_OUT_OF_MEMORY, NULL);
            status = QL_STATUS_OUT_OF_MEMORY;
            goto cleanup;
        }
        memcpy(instance->solver_options, text, length + 1u);
    }

cleanup:
    yyjson_doc_free(document);
    return status;
}

/* --- Capability ----------------------------------------------------------- */

/* Whether this method may claim a proof at all depends on the node's options,
   so a pipeline that never selects the trust policy cannot be scheduled as a
   proof producer. */
static ql_status QL_CALL smt_product_capability(
    const char *options_json, ql_proof_method_capability_v1 *capability,
    ql_error *error) {
    smt_product_instance probe;
    ql_status status;

    memset(&probe, 0, sizeof(probe));
    probe.allocator = *ql_default_allocator();
    status = parse_options(&probe, options_json, error);
    if (probe.solver_options != NULL) {
        probe.allocator.deallocate(probe.allocator.user_data,
                                   probe.solver_options);
    }
    if (status != QL_STATUS_OK) {
        return status;
    }
    capability->family = QL_PROOF_METHOD_FAMILY_SMT;
    capability->soundness_classes = QL_PROOF_SOUNDNESS_COUNTEREXAMPLE;
    capability->result_kinds =
        QL_PROOF_RESULT_COUNTEREXAMPLE | QL_PROOF_RESULT_UNKNOWN;
    if (probe.unsat_promotion == QL_UNSAT_PROMOTION_TRUSTED_BACKEND) {
        capability->soundness_classes |= QL_PROOF_SOUNDNESS_PROOF;
        capability->result_kinds |= QL_PROOF_RESULT_PROOF;
    }
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

typedef struct lowered_side {
    ql_c_frontend_unit *unit;
    ql_c_lower_result *result;
    ql_ir *ir;
} lowered_side;

static void lowered_side_dispose(lowered_side *side) {
    ql_ir_release(side->ir);
    ql_c_lower_result_destroy(side->result);
    ql_c_frontend_unit_destroy(side->unit);
    memset(side, 0, sizeof(*side));
}

/* A semantic limitation is reported through `supported`, never as a status
   failure and never as a proof about the function. */
static ql_status lower_side(const ql_allocator *allocator,
                            const char *source, size_t source_size,
                            const char *name, size_t name_size,
                            lowered_side *side, uint32_t *supported,
                            ql_error *error) {
    ql_c_function_view function;
    ql_c_lower_result_view_v1 view;
    ql_status status;

    QL_STAGE_MARK(stage_frontend);

    memset(side, 0, sizeof(*side));
    *supported = 0u;
    status = ql_c_frontend_analyze(allocator, source, source_size,
                                   &side->unit, error);
    QL_STAGE_ADD(QL_STAGE_FRONTEND, stage_frontend);
    if (status != QL_STATUS_OK) {
        return status;
    }
    {
        QL_STAGE_MARK(stage_select);
        memset(&function, 0, sizeof(function));
        function.struct_size = sizeof(function);
        status = ql_c_frontend_select_function(side->unit, name, name_size,
                                               &function, error);
        QL_STAGE_ADD(QL_STAGE_FRONTEND, stage_select);
    }
    if (status != QL_STATUS_OK) {
        return status;
    }
    {
        QL_STAGE_MARK(stage_lower);
        status = ql_c_lower_selected_function(allocator, source, source_size,
                                              side->unit, &function,
                                              &side->result, error);
        QL_STAGE_ADD(QL_STAGE_LOWER, stage_lower);
    }
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
    {
        QL_STAGE_MARK(stage_ir);
        status = ql_ir_open(allocator, view.ir_artifact, &side->ir, error);
        QL_STAGE_ADD(QL_STAGE_IR_OPEN, stage_ir);
    }
    if (status != QL_STATUS_OK) {
        return status;
    }
    *supported = 1u;
    return QL_STATUS_OK;
}

/* The loop encoder pairs source parameters by their IR order. A problem may
   legally bind a permutation, but applying that permutation without passing
   it to the encoder would invent equal entry states. Keep this fast path to
   positional bindings until the query API carries the full correspondence. */
static ql_status problem_has_positional_proof_binding(
    const ql_problem *problem, const ql_problem_view_v2 *view,
    uint32_t *matches, ql_error *error) {
    size_t index;
    ql_status status;

    *matches = 0u;
    status = ql_problem_require_proof_binding(problem, error);
    if (status != QL_STATUS_OK) {
        /* A missing proof binding is an eligibility boundary, not a malformed
           run. The outcome remains UNKNOWN and records that boundary. */
        ql_error_clear(error);
        return QL_STATUS_OK;
    }
    for (index = 0u; index < view->argument_binding_count; ++index) {
        ql_problem_argument_binding_v1 binding;
        memset(&binding, 0, sizeof(binding));
        binding.struct_size = sizeof(binding);
        status = ql_problem_argument_binding_at(problem, index, &binding,
                                                error);
        if (status != QL_STATUS_OK) {
            return status;
        }
        if (binding.left_index != binding.right_index) {
            ql_error_clear(error);
            return QL_STATUS_OK;
        }
    }
    *matches = 1u;
    ql_error_clear(error);
    return QL_STATUS_OK;
}

/* --- Solving -------------------------------------------------------------- */

/* The deadline this check may actually use: the method's own timeout and
   whatever the run has left, whichever is tighter.

   Cancellation alone cannot do this. The budget guard is polled between
   stages, so a run that goes over is noticed only when the solver returns,
   and the overrun is as long as the round trip. Measured: a one-millisecond
   deadline and a hundred-millisecond deadline both came back at ~0.41 s,
   which is where the first solver round trip ends, not where either deadline
   fell (docs/perf/concurrency.md). Handing the remaining time down as the
   subprocess deadline is what makes a tight budget cost less than a loose
   one.

   This can only tighten. A method that asked for a shorter timeout than the
   budget keeps it, and neither value can be widened by the other. */
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
        /* No time axis applies to this run. */
        return method_timeout_ms;
    }
    /* Round up, and never to zero while any time remains: the request reads
       zero as "no deadline", so truncating a live sub-millisecond budget would
       remove the limit rather than tighten it. budget.h states the same rule
       for ql_budget_remaining_ms. A budget already spent gets the smallest
       positive deadline instead, so the solver gives up at once rather than
       running unbounded; the guard turns that run into budget-exhausted
       regardless of what the solver answers. */
    remaining_ms = remaining_ns == 0u
                       ? 1u
                       : (remaining_ns + UINT64_C(999999)) / UINT64_C(1000000);
    if (method_timeout_ms == 0u || remaining_ms < method_timeout_ms) {
        return remaining_ms;
    }
    return method_timeout_ms;
}

static ql_status run_check(const ql_allocator *allocator,
                           const smt_product_instance *instance,
                           const ql_run_context_v1 *context,
                           const ql_product_query_view_v1 *query_view,
                           const ql_artifact *prefix,
                           const ql_artifact *terminal, uint32_t want_model,
                           ql_solver_check_result_v1 *result,
                           ql_error *error) {
    const ql_solver_descriptor_v1 *descriptor =
        ql_bitwuzla_solver_descriptor();
    ql_solver_check_request_v1 request;
    ql_solver *solver = NULL;
    ql_status status;
    QL_STAGE_MARK(stage_setup);

    if (descriptor->capability.availability == QL_SOLVER_UNAVAILABLE) {
        ql_error_set(error, QL_STATUS_NOT_FOUND,
                     "the canonical Bitwuzla backend is not available in this build");
        return QL_STATUS_NOT_FOUND;
    }
    /* A caller that judges many pairs establishes the backend once and puts
       the session on the run context; without one this builds its own, which
       is what every existing caller keeps doing. The session only supplies the
       installation: the request below, the digest verification inside the
       check, and the digest this result reports are identical either way. */
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
    if (want_model != 0u) {
        request.artifact_requests = QL_SOLVER_REQUEST_MODEL;
    }
    if (context != NULL && context->is_cancelled != NULL) {
        request.cancel_state = context->cancel_state;
        request.is_cancelled = context->is_cancelled;
    }
    ql_solver_check_result_init(result);
    QL_STAGE_ADD(QL_STAGE_SOLVER_SETUP, stage_setup);
    {
        QL_STAGE_MARK(stage_check);
        status = ql_solver_check(solver, &request, result, error);
        QL_STAGE_ADD(QL_STAGE_SOLVER_CHECK, stage_check);
    }
    {
        /* Teardown belongs with setup: both are the cost of having a backend
           rather than the cost of asking it something. */
        QL_STAGE_MARK(stage_teardown);
        ql_solver_destroy(solver);
        QL_STAGE_ADD(QL_STAGE_SOLVER_SETUP, stage_teardown);
    }
    return status;
}

/* --- Outcome serialization ------------------------------------------------ */

static int add_digest(yyjson_mut_doc *document, yyjson_mut_val *object,
                      const char *key, const ql_digest *digest) {
    char hex[QL_DIGEST_HEX_SIZE];
    ql_digest_hex(digest, hex);
    return yyjson_mut_obj_add_strcpy(document, object, key, hex);
}

static int add_loop_stats(yyjson_mut_doc *document, yyjson_mut_val *root,
                          const ql_loop_proof_stats_v1 *loop) {
    yyjson_mut_val *object = yyjson_mut_obj(document);
    yyjson_mut_val *stages = yyjson_mut_obj(document);
    yyjson_mut_val *obligations = yyjson_mut_obj(document);

    if (object == NULL || stages == NULL || obligations == NULL) {
        return 0;
    }
    if (!yyjson_mut_obj_add_uint(document, object, "schema_version",
                                 QL_LOOP_PROOF_STATS_SCHEMA_VERSION) ||
        !yyjson_mut_obj_add_bool(document, object, "applicable",
                                 loop->applicable != 0u) ||
        !yyjson_mut_obj_add_bool(document, object, "cyclic",
                                 loop->cyclic != 0u) ||
        !yyjson_mut_obj_add_bool(document, object, "noncanonical_cycle",
                                 loop->noncanonical_cycle != 0u) ||
        !yyjson_mut_obj_add_bool(document, object, "all_loops_paired",
                                 loop->all_loops_paired != 0u) ||
        !yyjson_mut_obj_add_bool(document, object, "fallback_reached",
                                 loop->fallback_reached != 0u) ||
        !yyjson_mut_obj_add_bool(document, object, "fallback_attempted",
                                 loop->fallback_attempted != 0u) ||
        !yyjson_mut_obj_add_bool(document, object, "proof_eligible",
                                 loop->proof_eligible != 0u) ||
        !yyjson_mut_obj_add_str(document, object, "strategy",
                                loop_strategy_string(loop->strategy)) ||
        !yyjson_mut_obj_add_str(document, object, "induction_answer",
                                answer_string(loop->induction_answer)) ||
        !yyjson_mut_obj_add_str(document, object, "summary_answer",
                                answer_string(loop->summary_answer)) ||
        !yyjson_mut_obj_add_str(document, object, "reflexivity_answer",
                                answer_string(loop->reflexivity_answer)) ||
        !yyjson_mut_obj_add_bool(document, object,
                                 "concrete_domain_witness",
                                 loop->concrete_domain_witness != 0u) ||
        !yyjson_mut_obj_add_uint(document, object, "left_loop_count",
                                 loop->left_loop_count) ||
        !yyjson_mut_obj_add_uint(document, object, "right_loop_count",
                                 loop->right_loop_count) ||
        !yyjson_mut_obj_add_uint(document, object, "natural_loop_count",
                                 loop->natural_loop_count) ||
        !yyjson_mut_obj_add_uint(document, object, "paired_loop_count",
                                 loop->paired_loop_count) ||
        !yyjson_mut_obj_add_uint(document, object,
                                 "invariant_generated_count",
                                 loop->invariant_generated_count) ||
        !yyjson_mut_obj_add_uint(document, object,
                                 "induction_proved_count",
                                 loop->induction_proved_count) ||
        !yyjson_mut_obj_add_uint(document, object,
                                 "summary_attempted_count",
                                 loop->summary_attempted_count) ||
        !yyjson_mut_obj_add_uint(document, object, "summary_proved_count",
                                 loop->summary_proved_count) ||
        !yyjson_mut_obj_add_uint(document, object,
                                 "reflexivity_proved_count",
                                 loop->reflexivity_proved_count) ||
        !yyjson_mut_obj_add_strcpy(document, object, "failure_reason",
                                   loop->failure_reason)) {
        return 0;
    }
    if (!yyjson_mut_obj_add_uint(document, stages, "reached",
                                 loop->stage_reached) ||
        !yyjson_mut_obj_add_uint(document, stages, "discover_ns",
                                 loop->discover_ns) ||
        !yyjson_mut_obj_add_uint(document, stages, "canonicalize_ns",
                                 loop->canonicalize_ns) ||
        !yyjson_mut_obj_add_uint(document, stages, "pairing_ns",
                                 loop->pairing_ns) ||
        !yyjson_mut_obj_add_uint(document, stages, "invariant_ns",
                                 loop->invariant_ns) ||
        !yyjson_mut_obj_add_uint(document, stages, "induction_ns",
                                 loop->induction_ns) ||
        !yyjson_mut_obj_add_uint(document, stages, "summary_ns",
                                 loop->summary_ns) ||
        !yyjson_mut_obj_add_uint(document, stages, "fallback_ns",
                                 loop->fallback_ns) ||
        !yyjson_mut_obj_add_uint(document, stages, "reflexivity_ns",
                                 loop->reflexivity_ns) ||
        !yyjson_mut_obj_add_val(document, object, "stages", stages)) {
        return 0;
    }
    if (!add_digest(document, obligations, "canonical",
                    &loop->canonical_digest) ||
        !add_digest(document, obligations, "base",
                    &loop->base_obligation_digest) ||
        !add_digest(document, obligations, "guard",
                    &loop->guard_obligation_digest) ||
        !add_digest(document, obligations, "step",
                    &loop->step_obligation_digest) ||
        !add_digest(document, obligations, "exit",
                    &loop->exit_obligation_digest) ||
        !add_digest(document, obligations, "reflexivity",
                    &loop->reflexivity_obligation_digest) ||
        !add_digest(document, obligations, "domain_witness",
                    &loop->domain_witness_digest) ||
        !yyjson_mut_obj_add_val(document, object, "digests", obligations) ||
        !yyjson_mut_obj_add_val(document, root, "loop_proof", object)) {
        return 0;
    }
    return 1;
}

static ql_status compute_cache_key(const ql_digest *problem_digest,
                                   const smt_product_instance *instance,
                                   const ql_product_query_view_v1 *query,
                                   const ql_loop_proof_stats_v1 *loop,
                                   const ql_digest *backend_digest,
                                   ql_digest *cache_key, ql_error *error) {
    /* Solver identity, exact options, and the canonical SMT-LIB bytes all
       participate, so a cached answer cannot be reused across a different
       backend, policy, or query. */
    char identity[2048];
    char prefix_hex[QL_DIGEST_HEX_SIZE];
    char violation_hex[QL_DIGEST_HEX_SIZE];
    char domain_hex[QL_DIGEST_HEX_SIZE];
    char backend_hex[QL_DIGEST_HEX_SIZE];
    char canonical_hex[QL_DIGEST_HEX_SIZE];
    char base_hex[QL_DIGEST_HEX_SIZE];
    char guard_hex[QL_DIGEST_HEX_SIZE];
    char step_hex[QL_DIGEST_HEX_SIZE];
    char exit_hex[QL_DIGEST_HEX_SIZE];
    char reflexivity_hex[QL_DIGEST_HEX_SIZE];
    char domain_witness_hex[QL_DIGEST_HEX_SIZE];
    char solver_options_hex[QL_DIGEST_HEX_SIZE];
    ql_digest solver_options_digest;
    ql_cache_key_input_v1 input;
    int written;

    ql_digest_hex(&query->prefix_digest, prefix_hex);
    ql_digest_hex(&query->violation_digest, violation_hex);
    ql_digest_hex(&query->domain_digest, domain_hex);
    ql_digest_hex(backend_digest, backend_hex);
    ql_digest_hex(&loop->canonical_digest, canonical_hex);
    ql_digest_hex(&loop->base_obligation_digest, base_hex);
    ql_digest_hex(&loop->guard_obligation_digest, guard_hex);
    ql_digest_hex(&loop->step_obligation_digest, step_hex);
    ql_digest_hex(&loop->exit_obligation_digest, exit_hex);
    ql_digest_hex(&loop->reflexivity_obligation_digest, reflexivity_hex);
    ql_digest_hex(&loop->domain_witness_digest, domain_witness_hex);
    ql_digest_data(instance->solver_options,
                   instance->solver_options != NULL
                       ? strlen(instance->solver_options)
                       : 0u,
                   &solver_options_digest);
    ql_digest_hex(&solver_options_digest, solver_options_hex);
    written = snprintf(
        identity, sizeof(identity),
        "{\"unsat_promotion\":\"%s\",\"timeout_ms\":%llu,\"memory_limit_mb\":%llu,\"solver_options\":\"%s\",\"prefix\":\"%s\",\"violation\":\"%s\",\"domain\":\"%s\",\"loop_canonical\":\"%s\",\"loop_base\":\"%s\",\"loop_guard\":\"%s\",\"loop_step\":\"%s\",\"loop_exit\":\"%s\",\"loop_reflexivity\":\"%s\",\"loop_domain_witness\":\"%s\",\"backend\":\"%s %s %s\"}",
        promotion_string(instance->unsat_promotion),
        (unsigned long long)instance->timeout_ms,
        (unsigned long long)instance->memory_limit_mb, solver_options_hex,
        prefix_hex,
        violation_hex, domain_hex, canonical_hex, base_hex, guard_hex,
        step_hex, exit_hex, reflexivity_hex, domain_witness_hex, "bitwuzla",
        QL_SMT_PRODUCT_METHOD_VERSION, backend_hex);
    if (written < 0 || (size_t)written >= sizeof(identity)) {
        ql_error_set(error, QL_STATUS_INTERNAL_ERROR,
                     "could not format the cache identity");
        return QL_STATUS_INTERNAL_ERROR;
    }
    memset(&input, 0, sizeof(input));
    input.struct_size = sizeof(input);
    input.artifact_digest = *problem_digest;
    input.semantic_problem_digest = *problem_digest;
    input.method_name = QL_SMT_PRODUCT_METHOD_NAME;
    input.method_version = QL_SMT_PRODUCT_METHOD_VERSION;
    input.canonical_options = identity;
    input.canonical_options_size = (size_t)written;
    return ql_cache_key_compute(&input, cache_key, error);
}

static ql_status build_outcome(const ql_allocator *allocator,
                               const smt_product_instance *instance,
                               const ql_problem_view_v2 *problem_view,
                               const ql_product_query_view_v1 *query_view,
                               const smt_product_decision *decision,
                               const ql_artifact *counterexample,
                               ql_artifact **output, ql_error *error) {
    ql_allocator allocator_copy = *allocator;
    yyjson_alc json_allocator = smt_json_allocator(&allocator_copy);
    yyjson_mut_doc *document = NULL;
    yyjson_mut_val *root;
    yyjson_mut_val *query_object;
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
                               query_view, &decision->loop,
                               &decision->solver_binary_digest, &cache_key,
                               error);
    if (status != QL_STATUS_OK) {
        return status;
    }

    document = yyjson_mut_doc_new(&json_allocator);
    root = document != NULL ? yyjson_mut_obj(document) : NULL;
    query_object = document != NULL ? yyjson_mut_obj(document) : NULL;
    solver_object = document != NULL ? yyjson_mut_obj(document) : NULL;
    trust_object = document != NULL ? yyjson_mut_obj(document) : NULL;
    if (document == NULL || root == NULL || query_object == NULL ||
        solver_object == NULL || trust_object == NULL ||
        !yyjson_mut_obj_add_str(document, root, "kind",
                                QL_ARTIFACT_KIND_OUTCOME) ||
        !yyjson_mut_obj_add_uint(document, root, "schema_version",
                                 QL_OUTCOME_SCHEMA_VERSION) ||
        !yyjson_mut_obj_add_str(document, root, "method",
                                QL_SMT_PRODUCT_METHOD_NAME) ||
        !yyjson_mut_obj_add_str(document, root, "method_version",
                                QL_SMT_PRODUCT_METHOD_VERSION) ||
        !yyjson_mut_obj_add_str(document, root, "verdict",
                                ql_verdict_string(decision->verdict)) ||
        !yyjson_mut_obj_add_str(document, root, "evidence_class",
                                ql_evidence_class_string(
                                    decision->evidence_class)) ||
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
    if (!add_loop_stats(document, root, &decision->loop)) {
        status = QL_STATUS_OUT_OF_MEMORY;
        ql_error_set(error, status, NULL);
        goto cleanup;
    }
    if (!add_digest(document, query_object, "prefix_digest",
                    &query_view->prefix_digest) ||
        !add_digest(document, query_object, "violation_digest",
                    &query_view->violation_digest) ||
        !add_digest(document, query_object, "domain_digest",
                    &query_view->domain_digest) ||
        !yyjson_mut_obj_add_str(document, query_object, "violation_answer",
                                answer_string(decision->violation_answer)) ||
        !yyjson_mut_obj_add_str(document, query_object, "domain_answer",
                                answer_string(decision->domain_answer)) ||
        !yyjson_mut_obj_add_val(document, root, "query", query_object)) {
        status = QL_STATUS_OUT_OF_MEMORY;
        ql_error_set(error, status, NULL);
        goto cleanup;
    }
    if (!yyjson_mut_obj_add_strcpy(document, solver_object, "name",
                                   decision->backend_name != NULL
                                       ? decision->backend_name
                                       : "") ||
        !yyjson_mut_obj_add_strcpy(document, solver_object, "version",
                                   decision->backend_version != NULL
                                       ? decision->backend_version
                                       : "") ||
        !add_digest(document, solver_object, "binary_digest",
                    &decision->solver_binary_digest) ||
        !add_digest(document, solver_object, "query_digest",
                    &decision->solver_query_digest) ||
        !yyjson_mut_obj_add_val(document, root, "solver", solver_object)) {
        status = QL_STATUS_OUT_OF_MEMORY;
        ql_error_set(error, status, NULL);
        goto cleanup;
    }
    /* The trust basis is part of the result, not a footnote: a PROVED verdict
       from this backend rests on trusting Bitwuzla, and the envelope says so. */
    if (!yyjson_mut_obj_add_str(document, trust_object, "unsat_promotion",
                                promotion_string(instance->unsat_promotion)) ||
        !yyjson_mut_obj_add_bool(document, trust_object, "checked_proof", 0) ||
        !yyjson_mut_obj_add_bool(document, trust_object, "replay_confirmed",
                                 decision->replay_confirmed != 0u) ||
        !yyjson_mut_obj_add_str(
            document, trust_object, "basis",
            decision->verdict == QL_VERDICT_COUNTEREXAMPLE
                ? "a decoded solver model reproduced the violation under concrete replay"
                : (decision->verdict == QL_VERDICT_UNKNOWN
                       ? "no sound conclusion was reached"
                       : "an explicit trusted-backend policy over a pinned Bitwuzla snapshot; no proof certificate was checked")) ||
        !yyjson_mut_obj_add_val(document, root, "trust", trust_object)) {
        status = QL_STATUS_OUT_OF_MEMORY;
        ql_error_set(error, status, NULL);
        goto cleanup;
    }
    if (counterexample != NULL) {
        if (!add_digest(document, root, "counterexample_digest",
                        &counterexample_view.digest) ||
            !yyjson_mut_obj_add_strncpy(
                document, root, "counterexample",
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
                                QL_OUTCOME_SCHEMA_VERSION, json, json_size,
                                output, error);

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

static ql_status QL_CALL smt_product_create(const ql_host_v1 *host,
                                            const char *options_json,
                                            void **instance,
                                            ql_error *error) {
    smt_product_instance *created;
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
    created->unsat_promotion = QL_UNSAT_PROMOTION_NONE;
    status = parse_options(created, options_json, error);
    if (status != QL_STATUS_OK) {
        allocator.deallocate(allocator.user_data, created->solver_options);
        allocator.deallocate(allocator.user_data, created);
        return status;
    }
    *instance = created;
    ql_error_clear(error);
    return QL_STATUS_OK;
}

static void QL_CALL smt_product_destroy(void *instance) {
    smt_product_instance *owned = (smt_product_instance *)instance;
    ql_allocator allocator;

    if (owned == NULL) {
        return;
    }
    allocator = owned->allocator;
    allocator.deallocate(allocator.user_data, owned->solver_options);
    allocator.deallocate(allocator.user_data, owned);
}

static ql_status QL_CALL smt_product_validate(void *instance,
                                              ql_artifact *const *inputs,
                                              size_t input_count,
                                              ql_error *error) {
    ql_artifact_view view;
    ql_status status;

    (void)instance;
    if (inputs == NULL || input_count != 1u || inputs[0] == NULL) {
        ql_error_set(error, QL_STATUS_INVALID_ARGUMENT,
                     "%s consumes exactly one quodlibet.problem artifact",
                     QL_SMT_PRODUCT_METHOD_NAME);
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
                     QL_SMT_PRODUCT_METHOD_NAME);
        return QL_STATUS_TYPE_MISMATCH;
    }
    /* Reject the unsupported axis before execution rather than during it. A
       schema v1 problem records no argument correspondence, so there is no
       relation between the two input lists for this method to encode. */
    if (view.schema_version != QL_PROBLEM_SCHEMA_VERSION_2) {
        ql_error_set(error, QL_STATUS_TYPE_MISMATCH,
                     "%s requires problem schema v2; schema v%u binds neither source signature nor the argument correspondence",
                     QL_SMT_PRODUCT_METHOD_NAME, view.schema_version);
        return QL_STATUS_TYPE_MISMATCH;
    }
    ql_error_clear(error);
    return QL_STATUS_OK;
}

/* Does either side thread an event trace? Shared with every other replaying
   method as ql_internal_ir_threads_event_trace; internal.h records the
   contract. */
#define ir_threads_an_event_trace ql_internal_ir_threads_event_trace

/* Second attempt at a replayable model, over the same violation claim with
   every object bounded. Only its SAT answer is used; the bounded query has no
   proof authority and this function never touches the verdict. A failure to
   find or replay one leaves `replay` inconclusive, which the caller reports as
   UNKNOWN. */
static ql_status retry_bounded(const ql_allocator *allocator,
                               const smt_product_instance *instance,
                               const ql_run_context_v1 *context,
                               const ql_product_query *query,
                               const ql_product_query_view_v1 *query_view,
                               const ql_problem *problem,
                               const ql_ir *left_ir, const ql_ir *right_ir,
                               ql_replay_witness **witness,
                               ql_replay_result_v1 *replay,
                               ql_error *error) {
    ql_solver_check_result_v1 bounded;
    ql_replay_witness *decoded = NULL;
    ql_status status;

    ql_solver_check_result_init(&bounded);
    status = run_check(allocator, instance, context, query_view,
                       ql_product_query_prefix_artifact(query),
                       ql_product_query_bounded_violation_artifact(query), 1u,
                       &bounded, error);
    if (status != QL_STATUS_OK) {
        ql_solver_check_result_clear(&bounded);
        return status;
    }
    if (bounded.kind != QL_SOLVER_CHECK_SAT ||
        bounded.model_artifact == NULL) {
        ql_solver_check_result_clear(&bounded);
        return QL_STATUS_OK;
    }
    status = ql_replay_decode_model(allocator, query, bounded.model_artifact,
                                    &decoded, error);
    if (status != QL_STATUS_OK) {
        QL_LOGE(QL_SMT_PRODUCT_CATEGORY,
                "bounded solver model could not be decoded: %s",
                error->message);
        ql_solver_check_result_clear(&bounded);
        ql_error_clear(error);
        return QL_STATUS_OK;
    }
    memset(replay, 0, sizeof(*replay));
    replay->struct_size = sizeof(*replay);
    status = ql_replay_execute(allocator, problem, query, left_ir, right_ir,
                               decoded, replay, error);
    if (status != QL_STATUS_OK) {
        ql_replay_witness_destroy(decoded);
        ql_solver_check_result_clear(&bounded);
        return status;
    }
    ql_replay_witness_destroy(*witness);
    *witness = decoded;
    ql_solver_check_result_clear(&bounded);
    return QL_STATUS_OK;
}

/* Decides the verdict from the two solver answers and, for SAT, from the
   concrete replay. Every path that does not reach a conclusion writes a
   diagnostic instead of settling for a weaker-looking verdict. */
static ql_status decide(const ql_allocator *allocator,
                        const smt_product_instance *instance,
                        const ql_run_context_v1 *context,
                        const ql_problem *problem,
                        const ql_product_query *query,
                        const ql_product_query_view_v1 *query_view,
                        const ql_ir *left_ir, const ql_ir *right_ir,
                        smt_product_decision *decision,
                        ql_artifact **counterexample, ql_error *error) {
    ql_solver_check_result_v1 violation;
    ql_solver_check_result_v1 domain;
    ql_replay_witness *witness = NULL;
    ql_replay_result_v1 replay;
    ql_status status;

    *counterexample = NULL;
    ql_solver_check_result_init(&violation);
    ql_solver_check_result_init(&domain);
    status = run_check(allocator, instance, context, query_view,
                       ql_product_query_prefix_artifact(query),
                       ql_product_query_violation_artifact(query), 1u,
                       &violation, error);
    if (status != QL_STATUS_OK) {
        return status;
    }
    decision->violation_answer = answer_from_solver(violation.kind);
    decision->solver_query_digest = violation.query_digest;
    decision->solver_binary_digest = violation.backend_binary_digest;
    decision->backend_name = violation.backend_name;
    decision->backend_version = violation.backend_version;

    if (violation.kind == QL_SOLVER_CHECK_SAT &&
        ir_threads_an_event_trace(left_ir, right_ir)) {
        /* The miter states external calls, so a violation over a body that
           calls is a real claim about the encoding. Confirming it is another
           matter: replay runs the IR concretely, and running a call needs a
           callee to run, which the witness does not carry. An unreplayed
           model is not a counterexample, so this stops at UNKNOWN and says
           why rather than promoting a model nothing has reproduced. */
        set_diagnostic(decision,
                       "the violation involves an external call, and replaying one needs a callee the witness does not carry, so no counterexample is claimed");
        goto finish;
    }
    if (violation.kind == QL_SOLVER_CHECK_SAT) {
        if (violation.model_artifact == NULL) {
            set_diagnostic(decision,
                           "the backend answered sat without a model, so no witness could be decoded");
            goto finish;
        }
        QL_STAGE_MARK(stage_replay);
        status = ql_replay_decode_model(allocator, query,
                                        violation.model_artifact, &witness,
                                        error);
        QL_STAGE_ADD(QL_STAGE_REPLAY, stage_replay);
        if (status != QL_STATUS_OK) {
            QL_LOGE(QL_SMT_PRODUCT_CATEGORY,
                    "solver model could not be decoded: %s", error->message);
            set_diagnostic(decision,
                           "the solver model could not be decoded into typed inputs");
            status = QL_STATUS_OK;
            goto finish;
        }
        memset(&replay, 0, sizeof(replay));
        replay.struct_size = sizeof(replay);
        {
            QL_STAGE_MARK(stage_execute);
            status = ql_replay_execute(allocator, problem, query, left_ir,
                                       right_ir, witness, &replay, error);
            QL_STAGE_ADD(QL_STAGE_REPLAY, stage_execute);
        }
        if (status != QL_STATUS_OK) {
            goto finish;
        }
        if (replay.conclusive == 0u &&
            ql_product_query_bounded_violation_artifact(query) != NULL) {
            /* The first model described objects too large to materialize.
               Asking again with every object bounded yields a violation the
               replay can actually run. A bounded SAT is still a real
               violation; a bounded UNSAT would prove nothing and is never
               read as one. */
            status = retry_bounded(allocator, instance, context, query,
                                   query_view, problem, left_ir, right_ir,
                                   &witness, &replay, error);
            if (status != QL_STATUS_OK) {
                goto finish;
            }
        }
        if (replay.conclusive == 0u) {
            set_diagnostic(decision,
                           "the decoded witness could not be replayed conclusively, so no counterexample is claimed");
            goto finish;
        }
        if (replay.violated == 0u) {
            /* The solver and the concrete semantics disagree. That is an
               encoding defect in this method, not a fact about the two
               functions, and it must be visible. */
            QL_LOGE(QL_SMT_PRODUCT_CATEGORY,
                    "solver reported sat but concrete replay did not reproduce the violation; the SMT encoding and the IR semantics disagree");
            set_diagnostic(decision,
                           "concrete replay did not reproduce the violation; this indicates an encoding defect and is reported as UNKNOWN");
            goto finish;
        }
        status = ql_replay_counterexample_artifact_create(
            allocator, query, witness, &replay, counterexample, error);
        if (status != QL_STATUS_OK) {
            goto finish;
        }
        decision->verdict = QL_VERDICT_COUNTEREXAMPLE;
        decision->evidence_class = QL_EVIDENCE_COUNTEREXAMPLE;
        decision->replay_confirmed = 1u;
        set_diagnostic(decision,
                       "a decoded solver model reproduced the violation under concrete replay");
        goto finish;
    }

    if (violation.kind != QL_SOLVER_CHECK_UNSAT) {
        set_diagnostic(decision,
                       "the backend did not decide the violation query");
        goto finish;
    }

    /* An UNSAT is only interesting once the domain it ranges over is known to
       be inhabited. */
    status = run_check(allocator, instance, context, query_view,
                       ql_product_query_prefix_artifact(query),
                       ql_product_query_domain_artifact(query), 0u, &domain,
                       error);
    if (status != QL_STATUS_OK) {
        goto finish;
    }
    decision->domain_answer = answer_from_solver(domain.kind);
    if (domain.kind != QL_SOLVER_CHECK_SAT) {
        set_diagnostic(decision,
                       domain.kind == QL_SOLVER_CHECK_UNSAT
                           ? "the precondition and UB policy leave an empty comparison domain, so this UNSAT is vacuous"
                           : "the backend did not decide whether the comparison domain is inhabited");
        goto finish;
    }
    if (instance->unsat_promotion != QL_UNSAT_PROMOTION_TRUSTED_BACKEND) {
        set_diagnostic(decision,
                       "solver UNSAT is retained as evidence only; no unsat_promotion policy was selected");
        goto finish;
    }
    status = ql_problem_require_proof_binding(problem, error);
    if (status != QL_STATUS_OK) {
        set_diagnostic(decision, error->message);
        status = QL_STATUS_OK;
        goto finish;
    }
    decision->verdict = proved_verdict_for(query_view->relation);
    decision->evidence_class = QL_EVIDENCE_PROOF;
    set_diagnostic(decision,
                   "the miter is unsatisfiable over an inhabited comparison domain under the selected trusted-backend policy");

finish:
    ql_replay_witness_destroy(witness);
    ql_solver_check_result_clear(&violation);
    ql_solver_check_result_clear(&domain);
    return status;
}

static void loop_stats_copy(smt_product_decision *decision,
                            const ql_loop_proof_query_view_v1 *view,
                            uint32_t declared_cyclic) {
    ql_loop_proof_stats_v1 *stats = &decision->loop;
    uint64_t natural_count = view->metrics.left_loop_count;

    if (view->metrics.right_loop_count > natural_count) {
        natural_count = view->metrics.right_loop_count;
    }
    stats->applicable =
        natural_count != 0u ||
        view->unsupported_reason ==
            QL_LOOP_PROOF_UNSUPPORTED_UNCLASSIFIED_CYCLE;
    stats->cyclic = declared_cyclic;
    stats->noncanonical_cycle =
        view->unsupported_reason ==
        QL_LOOP_PROOF_UNSUPPORTED_UNCLASSIFIED_CYCLE;
    stats->all_loops_paired = view->all_loops_paired;
    stats->fallback_reached = view->chc_pdr_reached;
    stats->fallback_attempted = view->chc_pdr_available;
    stats->proof_eligible = view->promotion_eligible;
    stats->strategy = view->strategy;
    stats->induction_answer = view->metrics.induction_answer;
    stats->summary_answer = view->metrics.summary_answer;
    stats->reflexivity_answer = view->metrics.reflexivity_answer;
    stats->concrete_domain_witness =
        view->metrics.concrete_domain_witness;
    stats->left_loop_count = view->metrics.left_loop_count;
    stats->right_loop_count = view->metrics.right_loop_count;
    stats->natural_loop_count = natural_count;
    stats->paired_loop_count = view->metrics.paired_loop_count;
    stats->invariant_generated_count =
        view->metrics.invariant_generated_count;
    stats->summary_attempted_count =
        view->metrics.summary_attempted_count;
    if (view->strategy == QL_LOOP_PROOF_STRATEGY_STRUCTURAL_INDUCTION &&
        view->metrics.induction_answer == QL_SMT_PRODUCT_ANSWER_UNSAT) {
        stats->induction_proved_count = view->metrics.paired_loop_count;
    }
    if (view->metrics.summary_answer == QL_SMT_PRODUCT_ANSWER_UNSAT) {
        stats->summary_proved_count = view->metrics.paired_loop_count;
    }
    if (view->strategy == QL_LOOP_PROOF_STRATEGY_EXACT_REFLEXIVITY &&
        view->metrics.reflexivity_answer == QL_SMT_PRODUCT_ANSWER_UNSAT) {
        stats->reflexivity_proved_count = view->metrics.paired_loop_count;
    }

    stats->stage_reached = view->metrics.stage_reached;
    stats->discover_ns = view->metrics.discover_ns;
    stats->canonicalize_ns = view->metrics.canonicalize_ns;
    stats->pairing_ns = view->metrics.pairing_ns;
    stats->invariant_ns =
        view->metrics.invariant_ns + view->metrics.query_build_ns;
    stats->induction_ns = view->metrics.induction_solver_ns;
    stats->summary_ns =
        view->metrics.summary_ns + view->metrics.summary_solver_ns;
    stats->fallback_ns = 0u;
    stats->reflexivity_ns = view->metrics.reflexivity_solver_ns;
    stats->canonical_digest = view->canonical_digest;
    stats->base_obligation_digest = view->base_obligation_digest;
    stats->guard_obligation_digest = view->guard_obligation_digest;
    stats->step_obligation_digest = view->step_obligation_digest;
    stats->exit_obligation_digest = view->exit_obligation_digest;
    stats->reflexivity_obligation_digest =
        view->reflexivity_obligation_digest;
    stats->domain_witness_digest = view->metrics.domain_witness_digest;
    (void)snprintf(stats->failure_reason, sizeof(stats->failure_reason), "%s",
                   view->diagnostic);
}

static void product_view_from_loop(
    const ql_problem_view_v2 *problem_view,
    const ql_loop_proof_query_view_v1 *loop_view,
    const ql_digest *terminal_digest, ql_product_query_view_v1 *view) {
    memset(view, 0, sizeof(*view));
    view->struct_size = sizeof(*view);
    view->schema_version = QL_PRODUCT_SCHEMA_VERSION;
    view->relation = problem_view->contract.relation;
    view->ub_policy = problem_view->contract.ub_policy;
    view->covered_observations = problem_view->contract.observations;
    view->logic = loop_view->logic;
    view->maximum_bv_width = loop_view->maximum_bv_width;
    view->problem_digest = problem_view->artifact_digest;
    view->prefix_digest = loop_view->prefix_digest;
    view->violation_digest = *terminal_digest;
    view->domain_digest = loop_view->domain_terminal_digest;
}

static void loop_solver_identity(smt_product_decision *decision,
                                 const ql_solver_check_result_v1 *result) {
    decision->solver_query_digest = result->query_digest;
    decision->solver_binary_digest = result->backend_binary_digest;
    decision->backend_name = result->backend_name;
    decision->backend_version = result->backend_version;
}

/* Runs the cheap cyclic dispatcher. `handled` is false only when neither IR
   contains an actual natural loop, in which case the ordinary acyclic product
   gets a chance to topologically order a conservatively CYCLIC declaration.

   SAT here refutes an invariant or summary candidate. It is never decoded,
   replayed, or reported as a program counterexample. */
static ql_status decide_loop(
    const ql_allocator *allocator, const smt_product_instance *instance,
    const ql_run_context_v1 *context, const ql_problem *problem,
    const ql_problem_view_v2 *problem_view, ql_loop_proof_query *query,
    uint32_t declared_cyclic, uint32_t *handled,
    ql_product_query_view_v1 *product_view,
    smt_product_decision *decision, ql_error *error) {
    ql_loop_proof_query_view_v1 loop_view;
    ql_solver_check_result_v1 proof;
    ql_solver_check_result_v1 domain;
    const ql_artifact *terminal = NULL;
    ql_loop_proof_check_target target = QL_LOOP_PROOF_CHECK_INDUCTION;
    ql_digest terminal_digest;
    uint64_t started_ns;
    ql_status status;

    *handled = 0u;
    memset(&loop_view, 0, sizeof(loop_view));
    loop_view.struct_size = sizeof(loop_view);
    status = ql_loop_proof_query_get_view(query, &loop_view, error);
    if (status != QL_STATUS_OK) {
        return status;
    }
    loop_stats_copy(decision, &loop_view, declared_cyclic);
    if (loop_view.disposition == QL_LOOP_PROOF_NOT_APPLICABLE) {
        return QL_STATUS_OK;
    }
    *handled = 1u;
    if (loop_view.disposition == QL_LOOP_PROOF_CHC_PDR_UNAVAILABLE) {
        memset(&terminal_digest, 0, sizeof(terminal_digest));
        product_view_from_loop(problem_view, &loop_view, &terminal_digest,
                               product_view);
        set_diagnostic(decision, loop_view.diagnostic);
        return QL_STATUS_OK;
    }

    if (loop_view.strategy == QL_LOOP_PROOF_STRATEGY_EXACT_REFLEXIVITY) {
        terminal = ql_loop_proof_query_reflexivity_artifact(query);
        terminal_digest = loop_view.reflexivity_terminal_digest;
        target = QL_LOOP_PROOF_CHECK_REFLEXIVITY;
    } else {
        terminal = ql_loop_proof_query_induction_artifact(query);
        terminal_digest = loop_view.induction_terminal_digest;
    }
    if (terminal == NULL &&
        ql_loop_proof_query_summary_artifact(query) != NULL) {
        terminal = ql_loop_proof_query_summary_artifact(query);
        terminal_digest = loop_view.summary_terminal_digest;
        target = QL_LOOP_PROOF_CHECK_SUMMARY;
    }
    if (terminal == NULL) {
        product_view_from_loop(problem_view, &loop_view, &terminal_digest,
                               product_view);
        set_diagnostic(decision, loop_view.diagnostic);
        return QL_STATUS_OK;
    }

    product_view_from_loop(problem_view, &loop_view, &terminal_digest,
                           product_view);
    ql_solver_check_result_init(&proof);
    ql_solver_check_result_init(&domain);
    started_ns = uv_hrtime();
    status = run_check(allocator, instance, context, product_view,
                       ql_loop_proof_query_prefix_artifact(query), terminal,
                       0u, &proof, error);
    if (status != QL_STATUS_OK) {
        goto finish;
    }
    loop_solver_identity(decision, &proof);
    decision->violation_answer = answer_from_solver(proof.kind);
    status = ql_loop_proof_query_record_check(
        query, target, proof.kind, proof.unknown_reason,
        uv_hrtime() - started_ns, error);
    if (status != QL_STATUS_OK) {
        goto finish;
    }

    memset(&loop_view, 0, sizeof(loop_view));
    loop_view.struct_size = sizeof(loop_view);
    status = ql_loop_proof_query_get_view(query, &loop_view, error);
    if (status != QL_STATUS_OK) {
        goto finish;
    }

    if (proof.kind == QL_SOLVER_CHECK_SAT &&
        target == QL_LOOP_PROOF_CHECK_INDUCTION &&
        ql_loop_proof_query_summary_artifact(query) != NULL) {
        ql_solver_check_result_clear(&proof);
        ql_solver_check_result_init(&proof);
        target = QL_LOOP_PROOF_CHECK_SUMMARY;
        terminal = ql_loop_proof_query_summary_artifact(query);
        terminal_digest = loop_view.summary_terminal_digest;
        product_view_from_loop(problem_view, &loop_view, &terminal_digest,
                               product_view);
        started_ns = uv_hrtime();
        status = run_check(allocator, instance, context, product_view,
                           ql_loop_proof_query_prefix_artifact(query), terminal,
                           0u, &proof, error);
        if (status != QL_STATUS_OK) {
            goto finish;
        }
        loop_solver_identity(decision, &proof);
        decision->violation_answer = answer_from_solver(proof.kind);
        status = ql_loop_proof_query_record_check(
            query, target, proof.kind, proof.unknown_reason,
            uv_hrtime() - started_ns, error);
        if (status != QL_STATUS_OK) {
            goto finish;
        }
        memset(&loop_view, 0, sizeof(loop_view));
        loop_view.struct_size = sizeof(loop_view);
        status = ql_loop_proof_query_get_view(query, &loop_view, error);
        if (status != QL_STATUS_OK) {
            goto finish;
        }
    }

    if (proof.kind != QL_SOLVER_CHECK_UNSAT) {
        loop_stats_copy(decision, &loop_view, declared_cyclic);
        set_diagnostic(decision, loop_view.diagnostic);
        status = QL_STATUS_OK;
        goto finish;
    }

    if (loop_view.promotion_eligible == 0u) {
        loop_stats_copy(decision, &loop_view, declared_cyclic);
        set_diagnostic(decision, loop_view.diagnostic);
        status = QL_STATUS_OK;
        goto finish;
    }

    if (loop_view.metrics.concrete_domain_witness != 0u) {
        decision->domain_answer = QL_SMT_PRODUCT_ANSWER_SAT;
    } else {
        started_ns = uv_hrtime();
        status = run_check(allocator, instance, context, product_view,
                           ql_loop_proof_query_prefix_artifact(query),
                           ql_loop_proof_query_domain_artifact(query), 0u,
                           &domain, error);
        if (status != QL_STATUS_OK) {
            goto finish;
        }
        decision->domain_answer = answer_from_solver(domain.kind);
        status = ql_loop_proof_query_record_check(
            query, QL_LOOP_PROOF_CHECK_DOMAIN, domain.kind,
            domain.unknown_reason, uv_hrtime() - started_ns, error);
        if (status != QL_STATUS_OK) {
            goto finish;
        }
    }
    memset(&loop_view, 0, sizeof(loop_view));
    loop_view.struct_size = sizeof(loop_view);
    status = ql_loop_proof_query_get_view(query, &loop_view, error);
    if (status != QL_STATUS_OK) {
        goto finish;
    }
    loop_stats_copy(decision, &loop_view, declared_cyclic);
    if (decision->domain_answer != QL_SMT_PRODUCT_ANSWER_SAT) {
        set_diagnostic(
            decision,
            decision->domain_answer == QL_SMT_PRODUCT_ANSWER_UNSAT
                ? "the loop comparison domain is empty, so the induction result is vacuous"
                : "the backend did not decide whether the loop comparison domain is inhabited");
        status = QL_STATUS_OK;
        goto finish;
    }
    if (instance->unsat_promotion != QL_UNSAT_PROMOTION_TRUSTED_BACKEND) {
        set_diagnostic(decision,
                       "loop-induction UNSAT is retained as evidence only; no unsat_promotion policy was selected");
        status = QL_STATUS_OK;
        goto finish;
    }
    status = ql_problem_require_proof_binding(problem, error);
    if (status != QL_STATUS_OK) {
        set_diagnostic(decision, error->message);
        ql_error_clear(error);
        status = QL_STATUS_OK;
        goto finish;
    }
    decision->verdict = proved_verdict_for(problem_view->contract.relation);
    decision->evidence_class = QL_EVIDENCE_PROOF;
    set_diagnostic(
        decision,
        target == QL_LOOP_PROOF_CHECK_SUMMARY
            ? "an affine loop summary is unsatisfiable over an inhabited comparison domain under the selected trusted-backend policy"
            : (target == QL_LOOP_PROOF_CHECK_REFLEXIVITY
                   ? "exact whole-IR identity proves reflexivity and a concrete defined execution inhabits the comparison domain under the selected trusted-backend policy"
                   : "Base, guard alignment, Step, and Exit are jointly unsatisfiable over an inhabited comparison domain under the selected trusted-backend policy"));
    status = QL_STATUS_OK;

finish:
    ql_solver_check_result_clear(&proof);
    ql_solver_check_result_clear(&domain);
    return status;
}

static ql_status QL_CALL smt_product_run(void *instance,
                                         const ql_run_context_v1 *context,
                                         ql_artifact *const *inputs,
                                         size_t input_count,
                                         ql_artifact **output,
                                         ql_error *error) {
    smt_product_instance *owned = (smt_product_instance *)instance;
    const ql_allocator *allocator;
    ql_problem *problem = NULL;
    ql_problem_view_v2 problem_view;
    ql_product_query *query = NULL;
    ql_loop_proof_query *loop_query = NULL;
    ql_product_query_view_v1 query_view;
    ql_artifact *counterexample = NULL;
    lowered_side left;
    lowered_side right;
    smt_product_decision decision;
    uint32_t left_supported = 0u;
    uint32_t right_supported = 0u;
    uint32_t loop_handled = 0u;
    ql_status status;
    QL_STAGE_MARK(stage_total);

    QL_STAGE_RESET();
    memset(&left, 0, sizeof(left));
    memset(&right, 0, sizeof(right));
    memset(&decision, 0, sizeof(decision));
    decision.loop.struct_size = sizeof(decision.loop);
    decision.loop.schema_version = QL_LOOP_PROOF_STATS_SCHEMA_VERSION;
    decision.verdict = QL_VERDICT_UNKNOWN;
    decision.evidence_class = QL_EVIDENCE_UNKNOWN;
    if (owned == NULL || context == NULL || output == NULL) {
        ql_error_set(error, QL_STATUS_INVALID_ARGUMENT,
                     "instance, run context, and output are required");
        return QL_STATUS_INVALID_ARGUMENT;
    }
    *output = NULL;
    status = smt_product_validate(instance, inputs, input_count, error);
    if (status != QL_STATUS_OK) {
        return status;
    }
    allocator = &owned->allocator;
    {
        QL_STAGE_MARK(stage_problem);
        status = ql_problem_open(allocator, inputs[0], &problem, error);
        if (status == QL_STATUS_OK) {
            memset(&problem_view, 0, sizeof(problem_view));
            problem_view.struct_size = sizeof(problem_view);
            status = ql_problem_get_view_v2(problem, &problem_view, error);
        }
        QL_STAGE_ADD(QL_STAGE_PROBLEM, stage_problem);
    }
    if (status != QL_STATUS_OK) {
        if (problem == NULL) {
            return status;
        }
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
                       "the semantic C lowering does not support one of the two functions, so no relation was encoded");
        memset(&query_view, 0, sizeof(query_view));
        query_view.struct_size = sizeof(query_view);
        query_view.relation = problem_view.contract.relation;
        query_view.ub_policy = problem_view.contract.ub_policy;
        query_view.covered_observations =
            problem_view.contract.observations;
        {
            QL_STAGE_MARK(stage_outcome);
            status = build_outcome(allocator, owned, &problem_view,
                                   &query_view, &decision, NULL, output,
                                   error);
            QL_STAGE_ADD(QL_STAGE_OUTCOME, stage_outcome);
        }
        goto cleanup;
    }

    {
        ql_ir_view_v1 left_ir_view;
        ql_ir_view_v1 right_ir_view;
        uint32_t declared_cyclic;

        memset(&left_ir_view, 0, sizeof(left_ir_view));
        memset(&right_ir_view, 0, sizeof(right_ir_view));
        left_ir_view.struct_size = sizeof(left_ir_view);
        right_ir_view.struct_size = sizeof(right_ir_view);
        status = ql_ir_get_view(left.ir, &left_ir_view, error);
        if (status == QL_STATUS_OK) {
            status = ql_ir_get_view(right.ir, &right_ir_view, error);
        }
        if (status != QL_STATUS_OK) {
            goto cleanup;
        }
        declared_cyclic =
            left_ir_view.cfg_kind == QL_IR_CFG_CYCLIC ||
                    right_ir_view.cfg_kind == QL_IR_CFG_CYCLIC
                ? 1u
                : 0u;
        if (declared_cyclic != 0u) {
            ql_loop_proof_options_v1 options;
            uint32_t binding_match = 0u;

            status = problem_has_positional_proof_binding(
                problem, &problem_view, &binding_match, error);
            if (status != QL_STATUS_OK) {
                goto cleanup;
            }
            ql_loop_proof_options_init(&options);
            options.precondition_is_true =
                problem_view.contract.precondition_json == NULL ? 1u : 0u;
            options.contract_binding_match = binding_match;
            status = ql_loop_proof_query_build(
                allocator, left.ir, right.ir, &options, &loop_query, error);
            if (status != QL_STATUS_OK) {
                goto cleanup;
            }
            status = decide_loop(allocator, owned, context, problem,
                                 &problem_view, loop_query, declared_cyclic,
                                 &loop_handled, &query_view, &decision, error);
            if (status != QL_STATUS_OK) {
                goto cleanup;
            }
            if (loop_handled != 0u) {
                if (decision.verdict == QL_VERDICT_UNKNOWN) {
                    (void)snprintf(decision.loop.failure_reason,
                                   sizeof(decision.loop.failure_reason), "%s",
                                   decision.diagnostic);
                } else {
                    decision.loop.failure_reason[0] = '\0';
                }
                QL_STAGE_MARK(stage_outcome);
                status = build_outcome(allocator, owned, &problem_view,
                                       &query_view, &decision, NULL, output,
                                       error);
                QL_STAGE_ADD(QL_STAGE_OUTCOME, stage_outcome);
                goto cleanup;
            }
        }
    }

    {
        QL_STAGE_MARK(stage_product);
        status = ql_product_query_build(allocator, problem, left.ir, right.ir,
                                        &query, error);
        QL_STAGE_ADD(QL_STAGE_PRODUCT, stage_product);
    }
    if (status == QL_STATUS_TYPE_MISMATCH) {
        /* An axis the miter cannot state is a boundary of this method, not a
           statement about the functions. */
        set_diagnostic(&decision, error->message);
        memset(&query_view, 0, sizeof(query_view));
        query_view.struct_size = sizeof(query_view);
        query_view.relation = problem_view.contract.relation;
        query_view.ub_policy = problem_view.contract.ub_policy;
        query_view.covered_observations =
            problem_view.contract.observations;
        {
            QL_STAGE_MARK(stage_outcome);
            status = build_outcome(allocator, owned, &problem_view,
                                   &query_view, &decision, NULL, output,
                                   error);
            QL_STAGE_ADD(QL_STAGE_OUTCOME, stage_outcome);
        }
        goto cleanup;
    }
    if (status != QL_STATUS_OK) {
        goto cleanup;
    }
    memset(&query_view, 0, sizeof(query_view));
    query_view.struct_size = sizeof(query_view);
    status = ql_product_query_get_view(query, &query_view, error);
    if (status != QL_STATUS_OK) {
        goto cleanup;
    }
    status = decide(allocator, owned, context, problem, query, &query_view,
                    left.ir, right.ir, &decision, &counterexample, error);
    if (status != QL_STATUS_OK) {
        goto cleanup;
    }
    {
        QL_STAGE_MARK(stage_outcome);
        status = build_outcome(allocator, owned, &problem_view, &query_view,
                               &decision, counterexample, output, error);
        QL_STAGE_ADD(QL_STAGE_OUTCOME, stage_outcome);
    }

cleanup:
    ql_artifact_release(counterexample);
    ql_loop_proof_query_destroy(loop_query);
    ql_product_query_destroy(query);
    lowered_side_dispose(&left);
    lowered_side_dispose(&right);
    ql_problem_release(problem);
    if (status == QL_STATUS_OK) {
        ql_error_clear(error);
    }
    /* The total covers the teardown above, which is where the two lowered
       sides and the query are freed. Leaving it out would hide a stage. */
    QL_STAGE_ADD(QL_STAGE_TOTAL, stage_total);
    QL_STAGE_EMIT();
    return status;
}

static const ql_method_v1 smt_product_method = {
    sizeof(ql_method_v1),
    QL_ABI_VERSION,
    QL_SMT_PRODUCT_METHOD_NAME,
    "Relational SMT product with structural loop induction",
    QL_ARTIFACT_KIND_OUTCOME,
    QL_METHOD_PROOF_PRODUCER | QL_METHOD_COUNTEREXAMPLE_PRODUCER |
        QL_METHOD_CACHEABLE,
    1u,
    1u,
    smt_product_create,
    smt_product_validate,
    smt_product_run,
    smt_product_destroy,
    { NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL }
};

static const ql_proof_method_v1 smt_product_descriptor = {
    sizeof(ql_proof_method_v1),
    QL_ABI_VERSION,
    QL_PROOF_METHOD_FAMILY_SMT,
    &smt_product_method,
    smt_product_capability,
    { NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL }
};

const ql_method_v1 *QL_CALL ql_smt_product_method(void) {
    return &smt_product_method;
}

const ql_proof_method_v1 *QL_CALL ql_smt_product_proof_method(void) {
    return &smt_product_descriptor;
}

ql_status QL_CALL ql_register_smt_product_method(ql_registry *registry,
                                                 ql_error *error) {
    ql_status status = ql_registry_register(registry, &smt_product_method,
                                            error);
    if (status != QL_STATUS_OK) {
        return status;
    }
    return ql_registry_register_proof_method(registry,
                                             &smt_product_descriptor, error);
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
        view->schema_version != QL_OUTCOME_SCHEMA_VERSION) {
        ql_error_set(error, QL_STATUS_TYPE_MISMATCH,
                     "artifact is not a schema v1 quodlibet.outcome");
        return QL_STATUS_TYPE_MISMATCH;
    }
    *document = yyjson_read_opts((char *)(uintptr_t)view->data, view->size,
                                 0u, NULL, &read_error);
    if (*document == NULL) {
        ql_error_set(error, QL_STATUS_PARSE_ERROR,
                     "invalid outcome JSON at byte %zu: %s", read_error.pos,
                     read_error.msg != NULL ? read_error.msg : "parse error");
        return QL_STATUS_PARSE_ERROR;
    }
    return QL_STATUS_OK;
}

ql_status QL_CALL ql_smt_product_outcome_read(
    const ql_artifact *artifact, ql_smt_product_outcome_view_v1 *view,
    ql_error *error) {
    ql_artifact_view artifact_view;
    yyjson_doc *document = NULL;
    yyjson_val *root;
    yyjson_val *query;
    yyjson_val *solver;
    yyjson_val *trust;
    yyjson_val *value;
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
    status = open_outcome_document(artifact, &artifact_view, &document,
                                   error);
    if (status != QL_STATUS_OK) {
        return status;
    }
    memset(view, 0, sizeof(*view));
    view->struct_size = sizeof(*view);
    view->schema_version = QL_OUTCOME_SCHEMA_VERSION;
    root = yyjson_doc_get_root(document);
    query = yyjson_obj_get(root, "query");
    solver = yyjson_obj_get(root, "solver");
    trust = yyjson_obj_get(root, "trust");
    if (!yyjson_is_obj(root) || !yyjson_is_obj(query) ||
        !yyjson_is_obj(solver) || !yyjson_is_obj(trust)) {
        ql_error_set(error, QL_STATUS_SCHEMA_MISMATCH,
                     "outcome JSON does not match schema version 1");
        status = QL_STATUS_SCHEMA_MISMATCH;
        goto cleanup;
    }
    text = yyjson_get_str(yyjson_obj_get(root, "verdict"));
    view->verdict = text != NULL ? verdict_parse(text) : QL_VERDICT_UNKNOWN;
    text = yyjson_get_str(yyjson_obj_get(root, "evidence_class"));
    view->evidence_class =
        text != NULL ? evidence_parse(text) : QL_EVIDENCE_UNKNOWN;
    text = yyjson_get_str(yyjson_obj_get(trust, "unsat_promotion"));
    view->unsat_promotion =
        (text != NULL && strcmp(text, "trusted-backend") == 0)
            ? QL_UNSAT_PROMOTION_TRUSTED_BACKEND
            : QL_UNSAT_PROMOTION_NONE;
    value = yyjson_obj_get(query, "violation_answer");
    view->violation_answer =
        yyjson_is_str(value)
            ? answer_parse(yyjson_get_str(value), yyjson_get_len(value))
            : QL_SMT_PRODUCT_ANSWER_NOT_QUERIED;
    value = yyjson_obj_get(query, "domain_answer");
    view->domain_answer =
        yyjson_is_str(value)
            ? answer_parse(yyjson_get_str(value), yyjson_get_len(value))
            : QL_SMT_PRODUCT_ANSWER_NOT_QUERIED;
    view->checked_proof =
        yyjson_get_bool(yyjson_obj_get(trust, "checked_proof")) ? 1u : 0u;
    view->replay_confirmed =
        yyjson_get_bool(yyjson_obj_get(trust, "replay_confirmed")) ? 1u : 0u;
    (void)read_digest_field(root, "problem_digest", &view->problem_digest);
    (void)read_digest_field(root, "cache_key", &view->cache_key);
    (void)read_digest_field(solver, "query_digest",
                            &view->solver_query_digest);
    (void)read_digest_field(solver, "binary_digest",
                            &view->solver_binary_digest);
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

static ql_loop_proof_strategy loop_strategy_parse(const char *text) {
    if (text == NULL) {
        return QL_LOOP_PROOF_STRATEGY_NONE;
    }
    if (strcmp(text, "structural-induction") == 0) {
        return QL_LOOP_PROOF_STRATEGY_STRUCTURAL_INDUCTION;
    }
    if (strcmp(text, "affine-summary") == 0) {
        return QL_LOOP_PROOF_STRATEGY_AFFINE_SUMMARY;
    }
    if (strcmp(text, "chc-pdr-unavailable") == 0) {
        return QL_LOOP_PROOF_STRATEGY_CHC_PDR_UNAVAILABLE;
    }
    if (strcmp(text, "exact-reflexivity") == 0) {
        return QL_LOOP_PROOF_STRATEGY_EXACT_REFLEXIVITY;
    }
    return QL_LOOP_PROOF_STRATEGY_NONE;
}

ql_status QL_CALL ql_smt_product_outcome_loop_stats(
    const ql_artifact *artifact, ql_loop_proof_stats_v1 *view,
    ql_error *error) {
    ql_artifact_view artifact_view;
    yyjson_doc *document = NULL;
    yyjson_val *object;
    yyjson_val *stages;
    yyjson_val *digests;
    yyjson_val *value;
    const char *text;
    ql_status status;

    if (artifact == NULL || view == NULL) {
        ql_error_set(error, QL_STATUS_INVALID_ARGUMENT,
                     "outcome artifact and loop stats view are required");
        return QL_STATUS_INVALID_ARGUMENT;
    }
    if (view->struct_size != 0u && view->struct_size < sizeof(*view)) {
        ql_error_set(error, QL_STATUS_ABI_MISMATCH,
                     "loop proof stats view structure is too small");
        return QL_STATUS_ABI_MISMATCH;
    }
    status = open_outcome_document(artifact, &artifact_view, &document,
                                   error);
    if (status != QL_STATUS_OK) {
        return status;
    }
    memset(view, 0, sizeof(*view));
    view->struct_size = sizeof(*view);
    view->schema_version = QL_LOOP_PROOF_STATS_SCHEMA_VERSION;
    object = yyjson_obj_get(yyjson_doc_get_root(document), "loop_proof");
    if (!yyjson_is_obj(object)) {
        ql_error_clear(error);
        yyjson_doc_free(document);
        return QL_STATUS_OK;
    }
    value = yyjson_obj_get(object, "schema_version");
    if (!yyjson_is_uint(value) ||
        yyjson_get_uint(value) != QL_LOOP_PROOF_STATS_SCHEMA_VERSION) {
        ql_error_set(error, QL_STATUS_SCHEMA_MISMATCH,
                     "loop proof stats do not use schema version 1");
        yyjson_doc_free(document);
        return QL_STATUS_SCHEMA_MISMATCH;
    }
    view->applicable = yyjson_get_bool(yyjson_obj_get(object, "applicable"));
    view->cyclic = yyjson_get_bool(yyjson_obj_get(object, "cyclic"));
    view->noncanonical_cycle =
        yyjson_get_bool(yyjson_obj_get(object, "noncanonical_cycle"));
    view->all_loops_paired =
        yyjson_get_bool(yyjson_obj_get(object, "all_loops_paired"));
    view->fallback_reached =
        yyjson_get_bool(yyjson_obj_get(object, "fallback_reached"));
    view->fallback_attempted =
        yyjson_get_bool(yyjson_obj_get(object, "fallback_attempted"));
    view->proof_eligible =
        yyjson_get_bool(yyjson_obj_get(object, "proof_eligible"));
    view->concrete_domain_witness =
        yyjson_get_bool(yyjson_obj_get(object, "concrete_domain_witness"));
    text = yyjson_get_str(yyjson_obj_get(object, "strategy"));
    view->strategy = loop_strategy_parse(text);
    value = yyjson_obj_get(object, "induction_answer");
    view->induction_answer =
        yyjson_is_str(value)
            ? answer_parse(yyjson_get_str(value), yyjson_get_len(value))
            : QL_SMT_PRODUCT_ANSWER_NOT_QUERIED;
    value = yyjson_obj_get(object, "summary_answer");
    view->summary_answer =
        yyjson_is_str(value)
            ? answer_parse(yyjson_get_str(value), yyjson_get_len(value))
            : QL_SMT_PRODUCT_ANSWER_NOT_QUERIED;
    value = yyjson_obj_get(object, "reflexivity_answer");
    view->reflexivity_answer =
        yyjson_is_str(value)
            ? answer_parse(yyjson_get_str(value), yyjson_get_len(value))
            : QL_SMT_PRODUCT_ANSWER_NOT_QUERIED;
#define QL_READ_LOOP_UINT(field_)                                           \
    do {                                                                    \
        value = yyjson_obj_get(object, #field_);                            \
        if (yyjson_is_uint(value)) {                                        \
            view->field_ = yyjson_get_uint(value);                          \
        }                                                                   \
    } while (0)
    QL_READ_LOOP_UINT(left_loop_count);
    QL_READ_LOOP_UINT(right_loop_count);
    QL_READ_LOOP_UINT(natural_loop_count);
    QL_READ_LOOP_UINT(paired_loop_count);
    QL_READ_LOOP_UINT(invariant_generated_count);
    QL_READ_LOOP_UINT(induction_proved_count);
    QL_READ_LOOP_UINT(summary_attempted_count);
    QL_READ_LOOP_UINT(summary_proved_count);
    QL_READ_LOOP_UINT(reflexivity_proved_count);
#undef QL_READ_LOOP_UINT
    stages = yyjson_obj_get(object, "stages");
    if (yyjson_is_obj(stages)) {
#define QL_READ_STAGE_UINT(json_name_, field_)                              \
        do {                                                                \
            value = yyjson_obj_get(stages, json_name_);                     \
            if (yyjson_is_uint(value)) {                                    \
                view->field_ = yyjson_get_uint(value);                      \
            }                                                               \
        } while (0)
        QL_READ_STAGE_UINT("reached", stage_reached);
        QL_READ_STAGE_UINT("discover_ns", discover_ns);
        QL_READ_STAGE_UINT("canonicalize_ns", canonicalize_ns);
        QL_READ_STAGE_UINT("pairing_ns", pairing_ns);
        QL_READ_STAGE_UINT("invariant_ns", invariant_ns);
        QL_READ_STAGE_UINT("induction_ns", induction_ns);
        QL_READ_STAGE_UINT("summary_ns", summary_ns);
        QL_READ_STAGE_UINT("fallback_ns", fallback_ns);
        QL_READ_STAGE_UINT("reflexivity_ns", reflexivity_ns);
#undef QL_READ_STAGE_UINT
    }
    digests = yyjson_obj_get(object, "digests");
    if (yyjson_is_obj(digests)) {
        (void)read_digest_field(digests, "canonical",
                                &view->canonical_digest);
        (void)read_digest_field(digests, "base",
                                &view->base_obligation_digest);
        (void)read_digest_field(digests, "guard",
                                &view->guard_obligation_digest);
        (void)read_digest_field(digests, "step",
                                &view->step_obligation_digest);
        (void)read_digest_field(digests, "exit",
                                &view->exit_obligation_digest);
        (void)read_digest_field(digests, "reflexivity",
                                &view->reflexivity_obligation_digest);
        (void)read_digest_field(digests, "domain_witness",
                                &view->domain_witness_digest);
    }
    text = yyjson_get_str(yyjson_obj_get(object, "failure_reason"));
    if (text != NULL) {
        (void)snprintf(view->failure_reason, sizeof(view->failure_reason),
                       "%s", text);
    }
    ql_error_clear(error);
    yyjson_doc_free(document);
    return QL_STATUS_OK;
}

ql_status QL_CALL ql_smt_product_outcome_counterexample(
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
    status = open_outcome_document(artifact, &artifact_view, &document,
                                   error);
    if (status != QL_STATUS_OK) {
        return status;
    }
    value = yyjson_obj_get(yyjson_doc_get_root(document), "counterexample");
    if (yyjson_is_str(value)) {
        status = ql_artifact_create(
            allocator, QL_ARTIFACT_KIND_COUNTEREXAMPLE,
            QL_REPLAY_SCHEMA_VERSION, yyjson_get_str(value),
            yyjson_get_len(value), output, error);
    } else {
        ql_error_clear(error);
    }
    yyjson_doc_free(document);
    return status;
}
