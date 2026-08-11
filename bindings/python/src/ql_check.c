#include "ql_check.h"

#include "quodlibet/c_frontend.h"
#include "quodlibet/c_lower.h"
#include "quodlibet/pipeline.h"
#include "quodlibet/problem.h"
#include "quodlibet/proof_bounded.h"
#include "quodlibet/proof_chcpdr.h"
#include "quodlibet/registry.h"
#include "quodlibet/scheduler.h"
#include "quodlibet/signature.h"
#include "quodlibet/solver.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define QL_PY_NODE_NAME "check"
#define QL_PY_OPTIONS_CAPACITY 4096u

char *ql_py_strdup(const char *text, size_t size) {
    char *copy;

    if (text == NULL) {
        return NULL;
    }
    copy = malloc(size + 1u);
    if (copy == NULL) {
        return NULL;
    }
    if (size != 0u) {
        memcpy(copy, text, size);
    }
    copy[size] = '\0';
    return copy;
}

uint32_t ql_py_backend_available(void) {
    return ql_bitwuzla_solver_descriptor()->capability.availability !=
                   QL_SOLVER_UNAVAILABLE
               ? 1u
               : 0u;
}

const char *ql_py_backend_path(void) { return ql_bitwuzla_executable_path(); }

void ql_py_spec_init(ql_py_spec *spec) {
    if (spec == NULL) {
        return;
    }
    memset(spec, 0, sizeof(*spec));
    ql_budget_limits_init(&spec->limits);
    spec->relation = QL_RELATION_EQUIVALENCE;
    spec->ub_policy = QL_UB_MUST_MATCH;
    spec->memory_observation = QL_MEMORY_FINAL_REACHABLE_STATE;
    spec->external_call_observation = QL_EXTERNAL_CALLS_ORDERED_TRACE;
}

void ql_py_spec_dispose(ql_py_spec *spec) {
    if (spec == NULL) {
        return;
    }
    free(spec->left_source);
    free(spec->left_function);
    free(spec->right_source);
    free(spec->right_function);
    free(spec->precondition_json);
    free(spec->solver_executable);
    free(spec->policy_json);
    free(spec->bindings);
    memset(spec, 0, sizeof(*spec));
}

void ql_py_result_init(ql_py_result *result) {
    if (result == NULL) {
        return;
    }
    memset(result, 0, sizeof(*result));
    result->verdict = QL_VERDICT_UNKNOWN;
    result->evidence_class = QL_EVIDENCE_UNKNOWN;
    result->loop_proof.struct_size = sizeof(result->loop_proof);
    result->loop_proof.schema_version = QL_LOOP_PROOF_STATS_SCHEMA_VERSION;
    ql_budget_usage_init(&result->usage);
    ql_policy_result_init(&result->policy);
}

void ql_py_result_dispose(ql_py_result *result) {
    if (result == NULL) {
        return;
    }
    free(result->counterexample_json);
    result->counterexample_json = NULL;
    result->counterexample_json_size = 0u;
}

static void fail(ql_py_result *result, const ql_error *error,
                 ql_status status) {
    result->ok = 0u;
    result->status = error != NULL && error->code != QL_STATUS_OK ? error->code
                                                                 : status;
    if (error != NULL && error->message[0] != '\0') {
        (void)snprintf(result->message, sizeof(result->message), "%s",
                       error->message);
    } else {
        (void)snprintf(result->message, sizeof(result->message), "%s",
                       ql_status_string(result->status));
    }
}

/* --- one lowered C function ---------------------------------------------- */

typedef struct ql_py_side {
    ql_c_frontend_unit *unit;
    ql_c_lower_result *lowered;
    ql_artifact *signature_artifact;
    const ql_artifact *ir_artifact;
    ql_c_lower_support support;
} ql_py_side;

static void side_dispose(ql_py_side *side) {
    ql_artifact_release(side->signature_artifact);
    ql_c_lower_result_destroy(side->lowered);
    ql_c_frontend_unit_destroy(side->unit);
    memset(side, 0, sizeof(*side));
}

static ql_status side_build(ql_py_side *side, const ql_allocator *allocator,
                            const char *source, size_t source_size,
                            const char *function_name, ql_error *error) {
    ql_c_function_view function;
    ql_c_lower_result_view_v1 view;
    ql_status status;

    memset(side, 0, sizeof(*side));
    side->support = QL_C_LOWER_UNKNOWN;
    status = ql_c_frontend_analyze(allocator, source, source_size, &side->unit,
                                   error);
    if (status != QL_STATUS_OK) {
        return status;
    }
    memset(&function, 0, sizeof(function));
    function.struct_size = sizeof(function);
    status = ql_c_frontend_select_function(side->unit, function_name,
                                           strlen(function_name), &function,
                                           error);
    if (status != QL_STATUS_OK) {
        return status;
    }
    status = ql_c_lower_selected_function(allocator, source, source_size,
                                          side->unit, &function,
                                          &side->lowered, error);
    if (status != QL_STATUS_OK) {
        return status;
    }
    memset(&view, 0, sizeof(view));
    view.struct_size = sizeof(view);
    status = ql_c_lower_result_get_view(side->lowered, &view, error);
    if (status != QL_STATUS_OK) {
        return status;
    }
    side->support = view.support;
    side->ir_artifact = view.ir_artifact;
    if (side->support != QL_C_LOWER_SUPPORTED) {
        /* Not an error. The caller turns this into UNKNOWN. */
        return QL_STATUS_OK;
    }
    /* The v2 form is given the source, so a parameter spelled with a typedef
       this unit declares resolves instead of being refused. */
    return ql_source_signature_from_c_function_v2(
        allocator, side->unit, &function, source, source_size,
        QL_C_DIALECT_ASM2C_GNU_V1, QL_TARGET_ABI_X86_64_LINUX_SYSV_LP64,
        &side->signature_artifact, error);
}

/* First lowering diagnostic, so an UNKNOWN says which construct stopped it. */
static void side_first_diagnostic(const ql_py_side *side, const char *label,
                                  char *buffer, size_t capacity) {
    ql_c_lower_diagnostic_view_v1 view;
    ql_error error;

    memset(&view, 0, sizeof(view));
    view.struct_size = sizeof(view);
    ql_error_clear(&error);
    if (side->lowered != NULL &&
        ql_c_lower_result_diagnostic_at(side->lowered, 0u, &view, &error) ==
            QL_STATUS_OK) {
        (void)snprintf(buffer, capacity,
                       "%s side is outside the restricted-C slice: %s (%s)",
                       label,
                       view.message != NULL ? view.message : "unsupported",
                       view.construct_kind != NULL ? view.construct_kind
                                                   : "construct");
        return;
    }
    (void)snprintf(buffer, capacity,
                   "%s side is outside the restricted-C slice", label);
}

/* --- method options ------------------------------------------------------ */

/* JSON string body escaping. Windows paths carry backslashes, so this is not
   optional. */
static ql_status json_escape(const char *text, char *buffer, size_t capacity,
                             size_t *written) {
    size_t index = 0u;
    size_t out = 0u;

    for (index = 0u; text[index] != '\0'; ++index) {
        const unsigned char character = (unsigned char)text[index];
        const char *escape = NULL;

        switch (character) {
        case '"':
            escape = "\\\"";
            break;
        case '\\':
            escape = "\\\\";
            break;
        case '\n':
            escape = "\\n";
            break;
        case '\r':
            escape = "\\r";
            break;
        case '\t':
            escape = "\\t";
            break;
        default:
            break;
        }
        if (character < 0x20u && escape == NULL) {
            return QL_STATUS_INVALID_ARGUMENT;
        }
        if (escape != NULL) {
            if (out + 2u >= capacity) {
                return QL_STATUS_INVALID_ARGUMENT;
            }
            buffer[out++] = escape[0];
            buffer[out++] = escape[1];
            continue;
        }
        if (out + 1u >= capacity) {
            return QL_STATUS_INVALID_ARGUMENT;
        }
        buffer[out++] = (char)character;
    }
    buffer[out] = '\0';
    *written = out;
    return QL_STATUS_OK;
}

/* The method takes its solver settings as a JSON string nested inside a JSON
   object, so the executable path is escaped twice. */
static ql_status build_options(const ql_py_spec *spec, char *buffer,
                               size_t capacity, ql_error *error) {
    char escaped_once[1024];
    char escaped_twice[2048];
    char nested[1200];
    size_t written = 0u;
    int printed;

    if (spec->solver_executable == NULL ||
        spec->solver_executable[0] == '\0') {
        printed = snprintf(
            buffer, capacity,
            "{\"unsat_promotion\":\"%s\",\"timeout_ms\":%llu,"
            "\"memory_limit_mb\":%llu}",
            spec->trust_smt_backend != 0u ? "trusted-backend" : "none",
            (unsigned long long)spec->solver_timeout_ms,
            (unsigned long long)spec->solver_memory_limit_mb);
        if (printed < 0 || (size_t)printed >= capacity) {
            ql_error_set(error, QL_STATUS_INVALID_ARGUMENT,
                         "method options do not fit");
            return QL_STATUS_INVALID_ARGUMENT;
        }
        return QL_STATUS_OK;
    }

    if (json_escape(spec->solver_executable, escaped_once,
                    sizeof(escaped_once), &written) != QL_STATUS_OK) {
        ql_error_set(error, QL_STATUS_INVALID_ARGUMENT,
                     "solver_executable cannot be encoded as JSON");
        return QL_STATUS_INVALID_ARGUMENT;
    }
    printed = snprintf(nested, sizeof(nested), "{\"executable\":\"%s\"}",
                       escaped_once);
    if (printed < 0 || (size_t)printed >= sizeof(nested)) {
        ql_error_set(error, QL_STATUS_INVALID_ARGUMENT,
                     "solver_executable is too long");
        return QL_STATUS_INVALID_ARGUMENT;
    }
    if (json_escape(nested, escaped_twice, sizeof(escaped_twice), &written) !=
        QL_STATUS_OK) {
        ql_error_set(error, QL_STATUS_INVALID_ARGUMENT,
                     "solver options cannot be encoded as JSON");
        return QL_STATUS_INVALID_ARGUMENT;
    }
    printed = snprintf(
        buffer, capacity,
        "{\"unsat_promotion\":\"%s\",\"timeout_ms\":%llu,"
        "\"memory_limit_mb\":%llu,\"solver_options\":\"%s\"}",
        spec->trust_smt_backend != 0u ? "trusted-backend" : "none",
        (unsigned long long)spec->solver_timeout_ms,
        (unsigned long long)spec->solver_memory_limit_mb, escaped_twice);
    if (printed < 0 || (size_t)printed >= capacity) {
        ql_error_set(error, QL_STATUS_INVALID_ARGUMENT,
                     "method options do not fit");
        return QL_STATUS_INVALID_ARGUMENT;
    }
    return QL_STATUS_OK;
}

/* --- the follow-up chain -------------------------------------------------- */

/* The nested solver_options field, `,"solver_options":"{...}"`, or an empty
   string when no executable override was given. The same double escaping
   build_options performs, factored for the follow-up methods' options. */
static ql_status build_solver_suffix(const ql_py_spec *spec, char *buffer,
                                     size_t capacity, ql_error *error) {
    char escaped_once[1024];
    char escaped_twice[2048];
    char nested[1200];
    size_t written = 0u;
    int printed;

    if (spec->solver_executable == NULL ||
        spec->solver_executable[0] == '\0') {
        if (capacity == 0u) {
            ql_error_set(error, QL_STATUS_INVALID_ARGUMENT,
                         "method options do not fit");
            return QL_STATUS_INVALID_ARGUMENT;
        }
        buffer[0] = '\0';
        return QL_STATUS_OK;
    }
    if (json_escape(spec->solver_executable, escaped_once,
                    sizeof(escaped_once), &written) != QL_STATUS_OK) {
        ql_error_set(error, QL_STATUS_INVALID_ARGUMENT,
                     "solver_executable cannot be encoded as JSON");
        return QL_STATUS_INVALID_ARGUMENT;
    }
    printed = snprintf(nested, sizeof(nested), "{\"executable\":\"%s\"}",
                       escaped_once);
    if (printed < 0 || (size_t)printed >= sizeof(nested)) {
        ql_error_set(error, QL_STATUS_INVALID_ARGUMENT,
                     "solver_executable is too long");
        return QL_STATUS_INVALID_ARGUMENT;
    }
    if (json_escape(nested, escaped_twice, sizeof(escaped_twice), &written) !=
        QL_STATUS_OK) {
        ql_error_set(error, QL_STATUS_INVALID_ARGUMENT,
                     "solver options cannot be encoded as JSON");
        return QL_STATUS_INVALID_ARGUMENT;
    }
    printed = snprintf(buffer, capacity, ",\"solver_options\":\"%s\"",
                       escaped_twice);
    if (printed < 0 || (size_t)printed >= capacity) {
        ql_error_set(error, QL_STATUS_INVALID_ARGUMENT,
                     "method options do not fit");
        return QL_STATUS_INVALID_ARGUMENT;
    }
    return QL_STATUS_OK;
}

/* One follow-up method over the same problem artifact, registry, budget and
   session. The pipeline outlives the result until the caller is done, which
   is why both handles come back together. */
static ql_status run_chain_method(const ql_py_spec *spec,
                                  const ql_allocator *allocator,
                                  ql_registry *registry,
                                  ql_scheduler *scheduler, ql_budget *budget,
                                  ql_artifact *problem_artifact,
                                  const char *method_name,
                                  const char *options, ql_pipeline **pipeline,
                                  ql_pipeline_result **run_result,
                                  ql_error *error) {
    ql_node_id node_id = QL_INVALID_NODE_ID;
    ql_status status;

    *pipeline = NULL;
    *run_result = NULL;
    status = ql_pipeline_create(registry, allocator, pipeline, error);
    if (status == QL_STATUS_OK) {
        status = ql_pipeline_add_node(*pipeline, QL_PY_NODE_NAME, method_name,
                                      options, NULL, 0u, &node_id, error);
    }
    if (status == QL_STATUS_OK && spec->solver_session != NULL) {
        status = ql_pipeline_set_solver_session(*pipeline,
                                                spec->solver_session, error);
    }
    if (status == QL_STATUS_OK) {
        status = ql_pipeline_compile(*pipeline, error);
    }
    if (status == QL_STATUS_OK) {
        status = ql_pipeline_run_with_budget(*pipeline, scheduler,
                                             problem_artifact, NULL, budget,
                                             run_result, error);
    }
    if (status == QL_STATUS_OK && ql_pipeline_result_count(*run_result) != 1u) {
        ql_error_set(error, QL_STATUS_INTERNAL_ERROR,
                     "the follow-up pipeline produced no outcome");
        status = QL_STATUS_INTERNAL_ERROR;
    }
    if (status != QL_STATUS_OK) {
        ql_pipeline_result_destroy(*run_result);
        ql_pipeline_destroy(*pipeline);
        *run_result = NULL;
        *pipeline = NULL;
    }
    return status;
}

/* search.bounded-symbolic, then prove.chc-pdr, entered only on an SMT-product
   UNKNOWN. The refuter runs first because a wrong candidate is the common
   case in the consumer this chain was built for, and its counterexample is
   final; a BOUNDED_CLEAN is adopted provisionally and a CHC/PDR proof
   overwrites it. Everything else leaves the result exactly as the SMT
   product said it. */
static ql_status run_followups(const ql_py_spec *spec,
                               const ql_allocator *allocator,
                               ql_registry *registry, ql_scheduler *scheduler,
                               ql_budget *budget,
                               ql_artifact *problem_artifact,
                               ql_py_result *result, ql_error *error) {
    char suffix[2400];
    char options[QL_PY_OPTIONS_CAPACITY];
    ql_pipeline *pipeline = NULL;
    ql_pipeline_result *run_result = NULL;
    ql_status status;
    int printed;

    status = build_solver_suffix(spec, suffix, sizeof(suffix), error);
    if (status != QL_STATUS_OK) {
        return status;
    }

    if (spec->bounded_unroll != 0u) {
        ql_bounded_outcome_view_v1 view;

        if (spec->solver_timeout_ms != 0u) {
            printed = snprintf(options, sizeof(options),
                               "{\"unroll_bound\":%llu,\"timeout_ms\":%llu%s}",
                               (unsigned long long)spec->bounded_unroll,
                               (unsigned long long)spec->solver_timeout_ms,
                               suffix);
        } else {
            printed = snprintf(options, sizeof(options),
                               "{\"unroll_bound\":%llu%s}",
                               (unsigned long long)spec->bounded_unroll,
                               suffix);
        }
        if (printed < 0 || (size_t)printed >= sizeof(options)) {
            ql_error_set(error, QL_STATUS_INVALID_ARGUMENT,
                         "bounded options do not fit");
            return QL_STATUS_INVALID_ARGUMENT;
        }
        status = run_chain_method(spec, allocator, registry, scheduler,
                                  budget, problem_artifact,
                                  QL_BOUNDED_METHOD_NAME, options, &pipeline,
                                  &run_result, error);
        if (status != QL_STATUS_OK) {
            return status;
        }
        memset(&view, 0, sizeof(view));
        view.struct_size = sizeof(view);
        status = ql_bounded_outcome_read(
            ql_pipeline_result_artifact(run_result, 0u), &view, error);
        if (status == QL_STATUS_OK &&
            (view.verdict == QL_VERDICT_COUNTEREXAMPLE ||
             view.verdict == QL_VERDICT_BOUNDED_CLEAN)) {
            result->verdict = view.verdict;
            result->evidence_class = view.evidence_class;
            result->checked_proof = 0u;
            result->replay_confirmed = view.replay_confirmed;
            (void)snprintf(result->diagnostic, sizeof(result->diagnostic),
                           "%s", view.diagnostic);
            ql_digest_hex(&view.cache_key, result->cache_key);
            ql_digest_hex(&view.counterexample_digest,
                          result->counterexample_digest);
            (void)snprintf(result->decided_by, sizeof(result->decided_by),
                           "%s", QL_BOUNDED_METHOD_NAME);
        }
        if (status == QL_STATUS_OK &&
            view.verdict == QL_VERDICT_COUNTEREXAMPLE) {
            ql_artifact *counterexample = NULL;

            status = ql_bounded_outcome_counterexample(
                allocator, ql_pipeline_result_artifact(run_result, 0u),
                &counterexample, error);
            if (status == QL_STATUS_OK && counterexample != NULL) {
                ql_artifact_view counterexample_view;

                memset(&counterexample_view, 0, sizeof(counterexample_view));
                counterexample_view.struct_size = sizeof(counterexample_view);
                status = ql_artifact_get_view(counterexample,
                                              &counterexample_view, error);
                if (status == QL_STATUS_OK) {
                    result->counterexample_json = ql_py_strdup(
                        (const char *)counterexample_view.data,
                        counterexample_view.size);
                    if (result->counterexample_json == NULL) {
                        ql_error_set(error, QL_STATUS_OUT_OF_MEMORY, NULL);
                        status = QL_STATUS_OUT_OF_MEMORY;
                    } else {
                        result->counterexample_json_size =
                            counterexample_view.size;
                    }
                }
            }
            ql_artifact_release(counterexample);
        }
        ql_pipeline_result_destroy(run_result);
        ql_pipeline_destroy(pipeline);
        if (status != QL_STATUS_OK) {
            return status;
        }
        if (result->verdict == QL_VERDICT_COUNTEREXAMPLE) {
            return QL_STATUS_OK;
        }
    }

    if (spec->chc_pdr != 0u) {
        ql_chcpdr_outcome_view_v1 view;

        if (spec->solver_timeout_ms != 0u) {
            printed = snprintf(
                options, sizeof(options),
                "{\"unsat_promotion\":\"%s\",\"timeout_ms\":%llu%s}",
                spec->trust_smt_backend != 0u ? "trusted-backend" : "none",
                (unsigned long long)spec->solver_timeout_ms, suffix);
        } else {
            printed = snprintf(
                options, sizeof(options), "{\"unsat_promotion\":\"%s\"%s}",
                spec->trust_smt_backend != 0u ? "trusted-backend" : "none",
                suffix);
        }
        if (printed < 0 || (size_t)printed >= sizeof(options)) {
            ql_error_set(error, QL_STATUS_INVALID_ARGUMENT,
                         "chc-pdr options do not fit");
            return QL_STATUS_INVALID_ARGUMENT;
        }
        status = run_chain_method(spec, allocator, registry, scheduler,
                                  budget, problem_artifact,
                                  QL_CHCPDR_METHOD_NAME, options, &pipeline,
                                  &run_result, error);
        if (status != QL_STATUS_OK) {
            return status;
        }
        memset(&view, 0, sizeof(view));
        view.struct_size = sizeof(view);
        status = ql_chcpdr_outcome_read(
            ql_pipeline_result_artifact(run_result, 0u), &view, error);
        if (status == QL_STATUS_OK &&
            (view.verdict == QL_VERDICT_PROVED_EQUIVALENT ||
             view.verdict == QL_VERDICT_PROVED_LEFT_REFINES_RIGHT ||
             view.verdict == QL_VERDICT_PROVED_RIGHT_REFINES_LEFT)) {
            result->verdict = view.verdict;
            result->evidence_class = view.evidence_class;
            result->checked_proof = 0u;
            result->replay_confirmed = 0u;
            result->unsat_promotion = spec->trust_smt_backend != 0u
                                          ? QL_UNSAT_PROMOTION_TRUSTED_BACKEND
                                          : QL_UNSAT_PROMOTION_NONE;
            (void)snprintf(result->diagnostic, sizeof(result->diagnostic),
                           "%s", view.diagnostic);
            ql_digest_hex(&view.cache_key, result->cache_key);
            (void)snprintf(result->decided_by, sizeof(result->decided_by),
                           "%s", QL_CHCPDR_METHOD_NAME);
        }
        ql_pipeline_result_destroy(run_result);
        ql_pipeline_destroy(pipeline);
        if (status != QL_STATUS_OK) {
            return status;
        }
    }
    return QL_STATUS_OK;
}

/* --- the run -------------------------------------------------------------- */

static void record_digests(ql_py_result *result,
                           const ql_smt_product_outcome_view_v1 *view) {
    ql_digest_hex(&view->problem_digest, result->problem_digest);
    ql_digest_hex(&view->solver_query_digest, result->solver_query_digest);
    ql_digest_hex(&view->solver_binary_digest, result->solver_binary_digest);
    ql_digest_hex(&view->cache_key, result->cache_key);
    ql_digest_hex(&view->counterexample_digest, result->counterexample_digest);
}

static ql_status apply_policy(const ql_policy *policy,
                              const ql_outcome_v1 *outcome,
                              ql_py_result *result, ql_error *error) {
    ql_policy_evidence_v1 evidence;
    char backend[128];

    ql_policy_evidence_init(&evidence);
    evidence.budget_exhausted = result->budget_exhausted;
    evidence.witness_replayed = result->replay_confirmed;
    evidence.proof_checked = result->checked_proof;
    (void)snprintf(backend, sizeof(backend), "%s %s",
                   ql_bitwuzla_solver_descriptor()->name,
                   ql_bitwuzla_solver_descriptor()->version);
    evidence.backend_identity = backend;
    result->has_policy = 1u;
    return ql_policy_evaluate(policy, outcome, &evidence, &result->policy,
                              error);
}

void ql_py_check(const ql_py_spec *spec, ql_py_result *result) {
    ql_error error;
    ql_budget *budget = NULL;
    const ql_allocator *allocator = NULL;
    ql_policy *policy = NULL;
    ql_py_side left;
    ql_py_side right;
    ql_artifact *problem_artifact = NULL;
    ql_registry *registry = NULL;
    ql_scheduler *scheduler = NULL;
    ql_pipeline *pipeline = NULL;
    ql_pipeline_result *run_result = NULL;
    ql_artifact *counterexample = NULL;
    ql_problem_argument_binding_v1 *bindings = NULL;
    size_t binding_count = 0u;
    ql_semantic_contract_v1 contract;
    ql_problem_definition_v2 definition;
    ql_smt_product_outcome_view_v1 view;
    ql_outcome_v1 outcome;
    ql_node_id node_id = QL_INVALID_NODE_ID;
    char options[QL_PY_OPTIONS_CAPACITY];
    ql_status status;

    ql_py_result_init(result);
    memset(&left, 0, sizeof(left));
    memset(&right, 0, sizeof(right));
    ql_error_clear(&error);

    if (spec == NULL || spec->left_source == NULL ||
        spec->right_source == NULL || spec->left_function == NULL ||
        spec->right_function == NULL) {
        ql_error_set(&error, QL_STATUS_INVALID_ARGUMENT,
                     "both sources and both function names are required");
        fail(result, &error, QL_STATUS_INVALID_ARGUMENT);
        return;
    }

    status = ql_budget_limits_validate(&spec->limits, &error);
    if (status != QL_STATUS_OK) {
        fail(result, &error, status);
        return;
    }
    status = ql_budget_create(NULL, &spec->limits, &budget, &error);
    if (status != QL_STATUS_OK) {
        fail(result, &error, status);
        return;
    }
    /* Every core allocation below is accounted against the memory axis. */
    allocator = ql_budget_allocator(budget);

    if (spec->policy_json != NULL) {
        status = ql_policy_parse(allocator, spec->policy_json,
                                 spec->policy_json_size, &policy, &error);
        if (status != QL_STATUS_OK) {
            goto failed;
        }
    }

    status = build_options(spec, options, sizeof(options), &error);
    if (status != QL_STATUS_OK) {
        goto failed;
    }

    status = side_build(&left, allocator, spec->left_source,
                        spec->left_source_size, spec->left_function, &error);
    if (status != QL_STATUS_OK) {
        goto failed;
    }
    status = side_build(&right, allocator, spec->right_source,
                        spec->right_source_size, spec->right_function,
                        &error);
    if (status != QL_STATUS_OK) {
        goto failed;
    }
    if (left.support != QL_C_LOWER_SUPPORTED ||
        right.support != QL_C_LOWER_SUPPORTED) {
        result->ok = 1u;
        result->outcome_kind = QL_PY_OUTCOME_UNSUPPORTED;
        result->verdict = QL_VERDICT_UNKNOWN;
        result->evidence_class = QL_EVIDENCE_UNKNOWN;
        side_first_diagnostic(left.support != QL_C_LOWER_SUPPORTED ? &left
                                                                  : &right,
                              left.support != QL_C_LOWER_SUPPORTED ? "left"
                                                                   : "right",
                              result->diagnostic, sizeof(result->diagnostic));
        goto finish;
    }

    /* Argument correspondence. The default is the identity over the left
       signature; ql_problem_artifact_create_v2 re-checks that it is a total
       bijection over both signatures. */
    if (spec->bindings != NULL) {
        binding_count = spec->binding_count;
    } else {
        ql_source_signature *signature = NULL;
        ql_source_signature_view_v1 signature_view;

        memset(&signature_view, 0, sizeof(signature_view));
        signature_view.struct_size = sizeof(signature_view);
        status = ql_source_signature_open(allocator, left.signature_artifact,
                                          &signature, &error);
        if (status == QL_STATUS_OK) {
            status = ql_source_signature_get_view(signature, &signature_view,
                                                  &error);
        }
        ql_source_signature_release(signature);
        if (status != QL_STATUS_OK) {
            goto failed;
        }
        binding_count = signature_view.argument_count;
    }
    if (binding_count != 0u) {
        size_t index;

        bindings = malloc(binding_count * sizeof(*bindings));
        if (bindings == NULL) {
            ql_error_set(&error, QL_STATUS_OUT_OF_MEMORY, NULL);
            status = QL_STATUS_OUT_OF_MEMORY;
            goto failed;
        }
        for (index = 0u; index < binding_count; ++index) {
            memset(&bindings[index], 0, sizeof(bindings[index]));
            bindings[index].struct_size = sizeof(bindings[index]);
            if (spec->bindings != NULL) {
                bindings[index].left_index = spec->bindings[index].left_index;
                bindings[index].right_index =
                    spec->bindings[index].right_index;
            } else {
                bindings[index].left_index = (uint32_t)index;
                bindings[index].right_index = (uint32_t)index;
            }
        }
    }

    ql_semantic_contract_init(&contract);
    contract.relation = spec->relation;
    contract.ub_policy = spec->ub_policy;
    if (spec->observations != 0u) {
        contract.observations = spec->observations;
        contract.memory_observation = spec->memory_observation;
        contract.external_call_observation = spec->external_call_observation;
    }
    if (spec->precondition_json != NULL) {
        contract.precondition_json = spec->precondition_json;
        contract.precondition_json_size = spec->precondition_json_size;
    }

    ql_problem_definition_v2_init(&definition);
    definition.contract = contract;
    definition.left_source = spec->left_source;
    definition.left_source_size = spec->left_source_size;
    definition.left_function_name = spec->left_function;
    definition.left_function_name_size = strlen(spec->left_function);
    definition.right_source = spec->right_source;
    definition.right_source_size = spec->right_source_size;
    definition.right_function_name = spec->right_function;
    definition.right_function_name_size = strlen(spec->right_function);
    definition.left_signature = left.signature_artifact;
    definition.right_signature = right.signature_artifact;
    definition.argument_bindings = bindings;
    definition.argument_binding_count = binding_count;
    status = ql_problem_artifact_create_v2(allocator, &definition,
                                           &problem_artifact, &error);
    if (status != QL_STATUS_OK) {
        goto failed;
    }

    status = ql_registry_create(allocator, &registry, &error);
    if (status == QL_STATUS_OK) {
        status = ql_register_smt_product_method(registry, &error);
    }
    /* The follow-up methods are registered up front even when the spec skips
       them: registration is cheap and a chain that fails to register at step
       0 beats one that fails after the SMT product already ran. */
    if (status == QL_STATUS_OK) {
        status = ql_register_bounded_method(registry, &error);
    }
    if (status == QL_STATUS_OK) {
        status = ql_register_chcpdr_method(registry, &error);
    }
    if (status == QL_STATUS_OK) {
        status = ql_pipeline_create(registry, allocator, &pipeline, &error);
    }
    if (status == QL_STATUS_OK) {
        status = ql_pipeline_add_node(pipeline, QL_PY_NODE_NAME,
                                      QL_SMT_PRODUCT_METHOD_NAME, options,
                                      NULL, 0u, &node_id, &error);
    }
    if (status == QL_STATUS_OK && spec->solver_session != NULL) {
        /* Lend the caller's backend installation to this run. Without it the
           method establishes one of its own, which is what a lone check still
           does. */
        status = ql_pipeline_set_solver_session(pipeline,
                                                spec->solver_session, &error);
    }
    if (status == QL_STATUS_OK) {
        status = ql_pipeline_compile(pipeline, &error);
    }
    if (status == QL_STATUS_OK) {
        status = ql_scheduler_create(allocator, 1u, &scheduler, &error);
    }
    if (status != QL_STATUS_OK) {
        goto failed;
    }

    ql_budget_start(budget);
    status = ql_pipeline_run_with_budget(pipeline, scheduler,
                                         problem_artifact, NULL, budget,
                                         &run_result, &error);
    result->budget_state = ql_budget_get_state(budget);
    if (status != QL_STATUS_OK) {
        goto failed;
    }
    if (ql_pipeline_result_count(run_result) != 1u) {
        ql_error_set(&error, QL_STATUS_INTERNAL_ERROR,
                     "the check pipeline produced no outcome");
        status = QL_STATUS_INTERNAL_ERROR;
        goto failed;
    }

    memset(&view, 0, sizeof(view));
    view.struct_size = sizeof(view);
    status = ql_smt_product_outcome_read(
        ql_pipeline_result_artifact(run_result, 0u), &view, &error);
    if (status != QL_STATUS_OK) {
        goto failed;
    }

    result->ok = 1u;
    result->outcome_kind = QL_PY_OUTCOME_METHOD;
    result->verdict = view.verdict;
    result->evidence_class = view.evidence_class;
    result->unsat_promotion = view.unsat_promotion;
    result->violation_answer = view.violation_answer;
    result->domain_answer = view.domain_answer;
    result->checked_proof = view.checked_proof;
    result->replay_confirmed = view.replay_confirmed;
    (void)snprintf(result->diagnostic, sizeof(result->diagnostic), "%s",
                   view.diagnostic);
    (void)snprintf(result->decided_by, sizeof(result->decided_by), "%s",
                   QL_SMT_PRODUCT_METHOD_NAME);
    record_digests(result, &view);

    status = ql_smt_product_outcome_loop_stats(
        ql_pipeline_result_artifact(run_result, 0u), &result->loop_proof,
        &error);
    if (status != QL_STATUS_OK) {
        goto failed;
    }

    status = ql_smt_product_outcome_counterexample(
        allocator, ql_pipeline_result_artifact(run_result, 0u),
        &counterexample, &error);
    if (status != QL_STATUS_OK) {
        goto failed;
    }
    if (counterexample != NULL) {
        ql_artifact_view counterexample_view;

        memset(&counterexample_view, 0, sizeof(counterexample_view));
        counterexample_view.struct_size = sizeof(counterexample_view);
        status = ql_artifact_get_view(counterexample, &counterexample_view,
                                      &error);
        if (status != QL_STATUS_OK) {
            goto failed;
        }
        /* Copied with the C runtime allocator so the buffer outlives the
           budget it was produced under. */
        result->counterexample_json = ql_py_strdup(
            (const char *)counterexample_view.data, counterexample_view.size);
        if (result->counterexample_json == NULL) {
            ql_error_set(&error, QL_STATUS_OUT_OF_MEMORY, NULL);
            status = QL_STATUS_OUT_OF_MEMORY;
            goto failed;
        }
        result->counterexample_json_size = counterexample_view.size;
    }

    /* The chain runs only where the SMT product failed to *answer*, not
       where an answer exists and a trust policy withheld the verdict: a
       violation UNSAT held back by `unsat_promotion` is strictly stronger
       evidence than anything the refuter could add, and a vacuous domain
       stays vacuous under every method. Budget keeps running through the
       chain, so the guard in `finish` still has the final word. */
    if (result->verdict == QL_VERDICT_UNKNOWN &&
        view.violation_answer != QL_SMT_PRODUCT_ANSWER_UNSAT &&
        view.domain_answer != QL_SMT_PRODUCT_ANSWER_UNSAT &&
        (spec->bounded_unroll != 0u || spec->chc_pdr != 0u) &&
        ql_budget_is_exhausted(budget) == 0u) {
        status = run_followups(spec, allocator, registry, scheduler, budget,
                               problem_artifact, result, &error);
        result->budget_state = ql_budget_get_state(budget);
        if (status != QL_STATUS_OK) {
            goto failed;
        }
    }
    goto finish;

failed:
    /* A limit can be reached long before the method runs, for instance while
       the memory axis is refusing an allocation inside the frontend. Every
       such failure is the budget's UNKNOWN, not an exception, because
       exhaustion is a run state rather than a defect in the request. */
    if (ql_budget_is_exhausted(budget) != 0u) {
        result->ok = 1u;
        result->outcome_kind = QL_PY_OUTCOME_BUDGET;
        result->verdict = QL_VERDICT_UNKNOWN;
        result->evidence_class = QL_EVIDENCE_UNKNOWN;
        result->budget_exhausted = 1u;
        result->budget_state = ql_budget_get_state(budget);
        (void)snprintf(result->diagnostic, sizeof(result->diagnostic),
                       "budget exhausted (%s): %s",
                       ql_budget_state_string(result->budget_state),
                       error.message[0] != '\0' ? error.message
                                                : "run stopped");
        goto finish;
    }
    fail(result, &error, status);
    goto cleanup;

finish:
    /* The budget gate runs even on the paths that never reached the method,
       so a run that hit a limit can never report anything but UNKNOWN. */
    memset(&outcome, 0, sizeof(outcome));
    outcome.struct_size = sizeof(outcome);
    outcome.schema_version = QL_OUTCOME_SCHEMA_VERSION;
    outcome.verdict = result->verdict;
    (void)ql_budget_guard_outcome(budget, &outcome, &error);
    result->verdict = outcome.verdict;
    if ((outcome.flags & QL_OUTCOME_FLAG_BUDGET_EXHAUSTED) != 0u) {
        result->budget_exhausted = 1u;
        result->outcome_kind = QL_PY_OUTCOME_BUDGET;
        result->evidence_class = QL_EVIDENCE_UNKNOWN;
    }
    result->budget_state = ql_budget_get_state(budget);
    (void)ql_budget_get_usage(budget, &result->usage, &error);

    if (policy != NULL) {
        ql_error policy_error;

        ql_error_clear(&policy_error);
        if (apply_policy(policy, &outcome, result, &policy_error) !=
            QL_STATUS_OK) {
            fail(result, &policy_error, QL_STATUS_INTERNAL_ERROR);
        }
    }
    ql_error_clear(&error);

cleanup:
    ql_artifact_release(counterexample);
    ql_pipeline_result_destroy(run_result);
    ql_pipeline_destroy(pipeline);
    ql_scheduler_destroy(scheduler);
    ql_registry_destroy(registry);
    ql_artifact_release(problem_artifact);
    free(bindings);
    side_dispose(&right);
    side_dispose(&left);
    ql_policy_destroy(policy);
    /* The budget owns the allocator every handle above was created with, so
       it is destroyed last. */
    ql_budget_destroy(budget);
    if (result->ok == 0u) {
        ql_py_result_dispose(result);
    }
}
