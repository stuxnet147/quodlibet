#include "quodlibet/combine.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

/* There is deliberately no tally of agreeing methods in this file, and no
   table of method priorities. The only things that decide anything are the
   validated evidence attached to an input and the soundness boundary of the
   verdict it carries. */

struct ql_combine_result {
    ql_allocator allocator;
    uint32_t inconsistent;
    uint32_t refinement_composition_applied;
    ql_verdict verdict;
    size_t deciding_input;
    size_t supporting_input;
    size_t conflicting_input;
    uint64_t accepted_count;
    uint64_t withdrawn_count;
    uint64_t bounded_count;
    ql_combine_finding_v1 *findings;
    size_t finding_count;
    ql_combine_bound_v1 *bounds;
    size_t bound_count;
};

static const ql_allocator *resolve_allocator(const ql_allocator *allocator) {
    if (allocator != NULL && ql_allocator_is_valid(allocator)) {
        return allocator;
    }
    return ql_default_allocator();
}

static int verdict_is_proof(ql_verdict verdict) {
    return verdict == QL_VERDICT_PROVED_EQUIVALENT ||
           verdict == QL_VERDICT_PROVED_LEFT_REFINES_RIGHT ||
           verdict == QL_VERDICT_PROVED_RIGHT_REFINES_LEFT;
}

/* Equivalence implies both refinements, so it settles any requested relation.
   A single direction settles only its own. */
static int proof_settles_relation(ql_verdict verdict, ql_relation relation) {
    if (verdict == QL_VERDICT_PROVED_EQUIVALENT) {
        return 1;
    }
    if (verdict == QL_VERDICT_PROVED_LEFT_REFINES_RIGHT) {
        return relation == QL_RELATION_LEFT_REFINES_RIGHT;
    }
    if (verdict == QL_VERDICT_PROVED_RIGHT_REFINES_LEFT) {
        return relation == QL_RELATION_RIGHT_REFINES_LEFT;
    }
    return 0;
}

static int backend_is_trusted(const ql_combine_request_v1 *request,
                              const char *backend) {
    size_t index;
    if (backend == NULL || backend[0] == '\0') {
        return 0;
    }
    for (index = 0u; index < request->trusted_backend_count; ++index) {
        const char *candidate = request->trusted_backends[index];
        if (candidate != NULL && strcmp(candidate, backend) == 0) {
            return 1;
        }
    }
    return 0;
}

static void set_finding(ql_combine_finding_v1 *finding, size_t index,
                        ql_combine_disposition disposition,
                        ql_combine_code code, ql_verdict reported,
                        ql_verdict effective, const char *format, ...) {
    va_list arguments;
    memset(finding, 0, sizeof(*finding));
    finding->input_index = index;
    finding->disposition = disposition;
    finding->code = code;
    finding->reported_verdict = reported;
    finding->effective_verdict = effective;
    va_start(arguments, format);
    vsnprintf(finding->detail, sizeof(finding->detail), format, arguments);
    va_end(arguments);
}

/* --- validation -------------------------------------------------------- */

static ql_status validate_request(const ql_combine_request_v1 *request,
                                  ql_error *error) {
    if (request == NULL) {
        ql_error_set(error, QL_STATUS_INVALID_ARGUMENT,
                     "a combine request is required");
        return QL_STATUS_INVALID_ARGUMENT;
    }
    if (request->struct_size != sizeof(*request)) {
        ql_error_set(error, QL_STATUS_ABI_MISMATCH,
                     "combine request struct_size is %zu, expected %zu",
                     request->struct_size, sizeof(*request));
        return QL_STATUS_ABI_MISMATCH;
    }
    if (request->abi_version != QL_ABI_VERSION) {
        ql_error_set(error, QL_STATUS_ABI_MISMATCH,
                     "combine request abi_version is %u, expected %u",
                     request->abi_version, (unsigned)QL_ABI_VERSION);
        return QL_STATUS_ABI_MISMATCH;
    }
    if (request->schema_version != QL_COMBINE_SCHEMA_VERSION) {
        ql_error_set(error, QL_STATUS_SCHEMA_MISMATCH,
                     "combine schema version is %u, expected %u",
                     request->schema_version,
                     (unsigned)QL_COMBINE_SCHEMA_VERSION);
        return QL_STATUS_SCHEMA_MISMATCH;
    }
    if (request->trusted_backend_count != 0u &&
        request->trusted_backends == NULL) {
        ql_error_set(error, QL_STATUS_INVALID_ARGUMENT,
                     "the request names %zu trusted backends and lists none",
                     request->trusted_backend_count);
        return QL_STATUS_INVALID_ARGUMENT;
    }
    return QL_STATUS_OK;
}

/* Rule 1. Answers to different questions are not combined; they are refused,
   and the message names the axis that differs so the caller can see which
   question each method actually answered. */
static ql_status validate_input(const ql_combine_request_v1 *request,
                                const ql_combine_input_v1 *input,
                                size_t index, size_t input_count,
                                ql_error *error) {
    if (input->struct_size != sizeof(*input)) {
        ql_error_set(error, QL_STATUS_ABI_MISMATCH,
                     "input %zu has struct_size %zu, expected %zu", index,
                     input->struct_size, sizeof(*input));
        return QL_STATUS_ABI_MISMATCH;
    }
    if (input->role != QL_COMBINE_ROLE_DECIDER &&
        input->role != QL_COMBINE_ROLE_NORMALIZER) {
        ql_error_set(error, QL_STATUS_INVALID_ARGUMENT,
                     "input %zu has role %u", index, input->role);
        return QL_STATUS_INVALID_ARGUMENT;
    }
    if (!ql_digest_equal(&input->problem_digest, &request->problem_digest)) {
        ql_error_set(error, QL_STATUS_SCHEMA_MISMATCH,
                     "input %zu answers a different problem digest", index);
        return QL_STATUS_SCHEMA_MISMATCH;
    }
    if (!ql_digest_equal(&input->contract_digest,
                         &request->contract_digest)) {
        ql_error_set(error, QL_STATUS_SCHEMA_MISMATCH,
                     "input %zu answers a different semantic contract digest",
                     index);
        return QL_STATUS_SCHEMA_MISMATCH;
    }
    if (input->ir_semantics_version != request->ir_semantics_version) {
        ql_error_set(error, QL_STATUS_SCHEMA_MISMATCH,
                     "input %zu used IR semantics version %u, the request "
                     "names %u", index, input->ir_semantics_version,
                     request->ir_semantics_version);
        return QL_STATUS_SCHEMA_MISMATCH;
    }
    if (input->relation != request->relation) {
        ql_error_set(error, QL_STATUS_TYPE_MISMATCH,
                     "input %zu answered relation %u, the request names %u",
                     index, (unsigned)input->relation,
                     (unsigned)request->relation);
        return QL_STATUS_TYPE_MISMATCH;
    }
    if (input->ub_policy != request->ub_policy) {
        ql_error_set(error, QL_STATUS_TYPE_MISMATCH,
                     "input %zu used UB policy %u, the request names %u",
                     index, (unsigned)input->ub_policy,
                     (unsigned)request->ub_policy);
        return QL_STATUS_TYPE_MISMATCH;
    }
    if (input->observations != request->observations ||
        input->memory_observation != request->memory_observation ||
        input->external_call_observation !=
            request->external_call_observation) {
        ql_error_set(error, QL_STATUS_TYPE_MISMATCH,
                     "input %zu used a different observation projection",
                     index);
        return QL_STATUS_TYPE_MISMATCH;
    }
    if (input->bound_count != 0u && input->bounds == NULL) {
        ql_error_set(error, QL_STATUS_INVALID_ARGUMENT,
                     "input %zu claims %zu bounds and lists none", index,
                     input->bound_count);
        return QL_STATUS_INVALID_ARGUMENT;
    }
    if (input->depends_on != QL_COMBINE_NO_INPUT) {
        if (input->depends_on >= input_count ||
            input->depends_on == index) {
            ql_error_set(error, QL_STATUS_INVALID_ARGUMENT,
                         "input %zu depends on %zu, which is not another "
                         "input", index, input->depends_on);
            return QL_STATUS_INVALID_ARGUMENT;
        }
        if (input->role == QL_COMBINE_ROLE_NORMALIZER) {
            ql_error_set(error, QL_STATUS_CYCLE,
                         "normalizer %zu may not itself depend on a "
                         "normalization", index);
            return QL_STATUS_CYCLE;
        }
    }
    return QL_STATUS_OK;
}

/* --- verdict validation ------------------------------------------------ */

/* Rule 2. The same discipline ql_policy_evaluate applies at the single-result
   boundary, applied again here because a combiner that trusted its inputs
   would be a way around it. */
static ql_verdict validate_verdict(const ql_combine_request_v1 *request,
                                   const ql_combine_input_v1 *input,
                                   ql_combine_code *code, char *detail,
                                   size_t detail_size) {
    const ql_verdict reported = input->outcome.verdict;
    *code = QL_COMBINE_CODE_NONE;
    detail[0] = '\0';

    if (input->evidence.budget_exhausted != 0u &&
        reported != QL_VERDICT_UNKNOWN) {
        *code = QL_COMBINE_CODE_BUDGET_EXHAUSTED;
        snprintf(detail, detail_size,
                 "the run exhausted its budget, so its %s is not a finished "
                 "answer", ql_verdict_string(reported));
        return QL_VERDICT_UNKNOWN;
    }
    if (verdict_is_proof(reported)) {
        if (input->evidence.proof_checked != 0u) {
            return reported;
        }
        if (backend_is_trusted(request, input->evidence.backend_identity)) {
            return reported;
        }
        if (input->evidence.backend_identity != NULL &&
            input->evidence.backend_identity[0] != '\0') {
            *code = QL_COMBINE_CODE_UNTRUSTED_BACKEND;
            snprintf(detail, detail_size,
                     "backend '%s' is not in the request's trusted list and "
                     "the proof was not checked",
                     input->evidence.backend_identity);
        } else {
            *code = QL_COMBINE_CODE_UNCHECKED_PROOF;
            snprintf(detail, detail_size,
                     "the proof was not checked and names no backend, so it "
                     "is a raw solver result");
        }
        return QL_VERDICT_UNKNOWN;
    }
    if (reported == QL_VERDICT_COUNTEREXAMPLE) {
        if (input->evidence.witness_replayed != 0u) {
            return reported;
        }
        *code = QL_COMBINE_CODE_UNREPLAYED_WITNESS;
        snprintf(detail, detail_size,
                 "the witness was never replayed, so this is a candidate "
                 "model and not a counterexample");
        return QL_VERDICT_UNKNOWN;
    }
    return reported;
}

/* --- deterministic ordering -------------------------------------------- */

/* Rule 8. Declaration order, then evidence digest, then input index. Worker
   completion time is never consulted. */
static int precedes(const ql_combine_input_v1 *inputs, size_t left,
                    size_t right) {
    const ql_combine_input_v1 *a = &inputs[left];
    const ql_combine_input_v1 *b = &inputs[right];
    int comparison;
    if (a->declaration_order != b->declaration_order) {
        return a->declaration_order < b->declaration_order;
    }
    comparison = memcmp(a->outcome.evidence_digest.bytes,
                        b->outcome.evidence_digest.bytes, QL_DIGEST_SIZE);
    if (comparison != 0) {
        return comparison < 0;
    }
    return left < right;
}

static void remember_best(const ql_combine_input_v1 *inputs, size_t candidate,
                          size_t *best) {
    if (*best == QL_COMBINE_NO_INPUT || precedes(inputs, candidate, *best)) {
        *best = candidate;
    }
}

/* --- evaluation -------------------------------------------------------- */

static void destroy_result(ql_combine_result *result) {
    ql_allocator allocator;
    if (result == NULL) {
        return;
    }
    allocator = result->allocator;
    allocator.deallocate(allocator.user_data, result->findings);
    allocator.deallocate(allocator.user_data, result->bounds);
    allocator.deallocate(allocator.user_data, result);
}

void QL_CALL ql_combine_request_init(ql_combine_request_v1 *request) {
    if (request == NULL) {
        return;
    }
    memset(request, 0, sizeof(*request));
    request->struct_size = sizeof(*request);
    request->abi_version = QL_ABI_VERSION;
    request->schema_version = QL_COMBINE_SCHEMA_VERSION;
    request->relation = QL_RELATION_EQUIVALENCE;
    request->ub_policy = QL_UB_MUST_MATCH;
    request->observations = QL_OBSERVE_RETURN_VALUE;
    request->memory_observation = QL_MEMORY_IGNORE;
    request->external_call_observation = QL_EXTERNAL_CALLS_IGNORE;
}

void QL_CALL ql_combine_input_init(ql_combine_input_v1 *input) {
    if (input == NULL) {
        return;
    }
    memset(input, 0, sizeof(*input));
    input->struct_size = sizeof(*input);
    input->relation = QL_RELATION_EQUIVALENCE;
    input->ub_policy = QL_UB_MUST_MATCH;
    input->observations = QL_OBSERVE_RETURN_VALUE;
    input->memory_observation = QL_MEMORY_IGNORE;
    input->external_call_observation = QL_EXTERNAL_CALLS_IGNORE;
    input->role = QL_COMBINE_ROLE_DECIDER;
    input->depends_on = QL_COMBINE_NO_INPUT;
    input->outcome.struct_size = sizeof(input->outcome);
    input->outcome.schema_version = 1u;
    input->outcome.verdict = QL_VERDICT_UNKNOWN;
    ql_policy_evidence_init(&input->evidence);
}

static size_t count_bounds(const ql_combine_input_v1 *inputs,
                           size_t input_count, const ql_verdict *effective) {
    size_t total = 0u;
    size_t index;
    for (index = 0u; index < input_count; ++index) {
        if (effective[index] == QL_VERDICT_BOUNDED_CLEAN) {
            total += inputs[index].bound_count;
        }
    }
    return total;
}

ql_status QL_CALL ql_combine_evaluate(
    const ql_allocator *allocator, const ql_combine_request_v1 *request,
    const ql_combine_input_v1 *inputs, size_t input_count,
    ql_combine_result **result, ql_error *error) {
    const ql_allocator *actual = resolve_allocator(allocator);
    ql_combine_result *created = NULL;
    ql_verdict *effective = NULL;
    ql_status status;
    size_t index;
    size_t bound_total;
    size_t bound_offset = 0u;
    size_t settling_proof = QL_COMBINE_NO_INPUT;
    size_t left_refinement = QL_COMBINE_NO_INPUT;
    size_t right_refinement = QL_COMBINE_NO_INPUT;
    size_t counterexample = QL_COMBINE_NO_INPUT;

    if (result == NULL) {
        ql_error_set(error, QL_STATUS_INVALID_ARGUMENT,
                     "a combine result output is required");
        return QL_STATUS_INVALID_ARGUMENT;
    }
    *result = NULL;
    status = validate_request(request, error);
    if (status != QL_STATUS_OK) {
        return status;
    }
    if (input_count == 0u || inputs == NULL) {
        ql_error_set(error, QL_STATUS_INVALID_ARGUMENT,
                     "combining needs at least one input");
        return QL_STATUS_INVALID_ARGUMENT;
    }
    for (index = 0u; index < input_count; ++index) {
        status = validate_input(request, &inputs[index], index, input_count,
                                error);
        if (status != QL_STATUS_OK) {
            return status;
        }
    }

    created = actual->allocate(actual->user_data, sizeof(*created));
    effective = actual->allocate(actual->user_data,
                                 input_count * sizeof(*effective));
    if (created == NULL || effective == NULL) {
        actual->deallocate(actual->user_data, created);
        actual->deallocate(actual->user_data, effective);
        ql_error_set(error, QL_STATUS_OUT_OF_MEMORY,
                     "could not allocate the combination state");
        return QL_STATUS_OUT_OF_MEMORY;
    }
    memset(created, 0, sizeof(*created));
    created->allocator = *actual;
    created->verdict = QL_VERDICT_UNKNOWN;
    created->deciding_input = QL_COMBINE_NO_INPUT;
    created->supporting_input = QL_COMBINE_NO_INPUT;
    created->conflicting_input = QL_COMBINE_NO_INPUT;
    created->findings = actual->allocate(
        actual->user_data, input_count * sizeof(*created->findings));
    if (created->findings == NULL) {
        actual->deallocate(actual->user_data, effective);
        destroy_result(created);
        ql_error_set(error, QL_STATUS_OUT_OF_MEMORY,
                     "could not allocate the combination findings");
        return QL_STATUS_OUT_OF_MEMORY;
    }
    created->finding_count = input_count;

    for (index = 0u; index < input_count; ++index) {
        ql_combine_code code = QL_COMBINE_CODE_NONE;
        char detail[QL_COMBINE_DETAIL_CAPACITY];
        effective[index] =
            validate_verdict(request, &inputs[index], &code, detail,
                             sizeof(detail));
        set_finding(&created->findings[index], index,
                    QL_COMBINE_DISPOSITION_NEUTRAL, code,
                    inputs[index].outcome.verdict, effective[index], "%s",
                    detail);
    }

    /* Rule 7. A decider that ran on a normalization can only be transferred
       back to the original problem when the normalization's own proof is
       present and valid. */
    for (index = 0u; index < input_count; ++index) {
        const ql_combine_input_v1 *input = &inputs[index];
        if (input->depends_on == QL_COMBINE_NO_INPUT ||
            effective[index] == QL_VERDICT_UNKNOWN) {
            continue;
        }
        if (inputs[input->depends_on].role != QL_COMBINE_ROLE_NORMALIZER) {
            actual->deallocate(actual->user_data, effective);
            destroy_result(created);
            ql_error_set(error, QL_STATUS_INVALID_ARGUMENT,
                         "input %zu depends on %zu, which is not a "
                         "normalizer", index, input->depends_on);
            return QL_STATUS_INVALID_ARGUMENT;
        }
        if (effective[input->depends_on] != QL_VERDICT_PROVED_EQUIVALENT) {
            set_finding(&created->findings[index], index,
                        QL_COMBINE_DISPOSITION_NEUTRAL,
                        QL_COMBINE_CODE_BROKEN_NORMALIZATION,
                        input->outcome.verdict, QL_VERDICT_UNKNOWN,
                        "normalization %zu did not establish an equivalence, "
                        "so this result cannot be transferred back to the "
                        "original problem", input->depends_on);
            effective[index] = QL_VERDICT_UNKNOWN;
        }
    }

    for (index = 0u; index < input_count; ++index) {
        const ql_combine_input_v1 *input = &inputs[index];
        ql_combine_finding_v1 *finding = &created->findings[index];
        const ql_verdict value = effective[index];
        finding->effective_verdict = value;

        if (value == QL_VERDICT_UNKNOWN) {
            if (input->outcome.verdict == QL_VERDICT_UNKNOWN) {
                finding->disposition = QL_COMBINE_DISPOSITION_NEUTRAL;
            } else {
                finding->disposition = QL_COMBINE_DISPOSITION_WITHDRAWN;
                ++created->withdrawn_count;
            }
            continue;
        }
        if (value == QL_VERDICT_BOUNDED_CLEAN) {
            finding->disposition = QL_COMBINE_DISPOSITION_BOUNDED;
            ++created->bounded_count;
            continue;
        }
        if (input->role == QL_COMBINE_ROLE_NORMALIZER) {
            /* A normalization proof is about the transformation, not about
               the requested relation, so it never decides on its own. */
            finding->disposition = QL_COMBINE_DISPOSITION_ACCEPTED;
            ++created->accepted_count;
            continue;
        }
        finding->disposition = QL_COMBINE_DISPOSITION_ACCEPTED;
        ++created->accepted_count;
        if (value == QL_VERDICT_COUNTEREXAMPLE) {
            remember_best(inputs, index, &counterexample);
            continue;
        }
        if (proof_settles_relation(value, request->relation)) {
            remember_best(inputs, index, &settling_proof);
            continue;
        }
        /* A proof of one direction when equivalence was asked for. It is
           valid evidence and is kept, but it does not settle the request on
           its own. */
        if (value == QL_VERDICT_PROVED_LEFT_REFINES_RIGHT) {
            remember_best(inputs, index, &left_refinement);
        } else if (value == QL_VERDICT_PROVED_RIGHT_REFINES_LEFT) {
            remember_best(inputs, index, &right_refinement);
        }
        finding->code = QL_COMBINE_CODE_WEAKER_RELATION;
        snprintf(finding->detail, sizeof(finding->detail),
                 "%s does not settle the requested relation on its own",
                 ql_verdict_string(value));
    }

    /* Rule 3, second half. Two checked opposite refinements compose into an
       equivalence only because the caller asked for that rule to be applied;
       everything else it requires is already forced by the contract match. */
    if (settling_proof == QL_COMBINE_NO_INPUT &&
        request->relation == QL_RELATION_EQUIVALENCE &&
        request->allow_refinement_composition != 0u &&
        left_refinement != QL_COMBINE_NO_INPUT &&
        right_refinement != QL_COMBINE_NO_INPUT) {
        created->refinement_composition_applied = 1u;
    }

    {
        const int has_proof =
            settling_proof != QL_COMBINE_NO_INPUT ||
            created->refinement_composition_applied != 0u;
        if (has_proof && counterexample != QL_COMBINE_NO_INPUT) {
            /* Rule 4. Not resolved by priority, ordering, or counting. */
            created->inconsistent = 1u;
            created->verdict = QL_VERDICT_UNKNOWN;
            created->deciding_input = settling_proof != QL_COMBINE_NO_INPUT
                                          ? settling_proof
                                          : left_refinement;
            created->supporting_input =
                created->refinement_composition_applied != 0u
                    ? right_refinement
                    : QL_COMBINE_NO_INPUT;
            created->conflicting_input = counterexample;
        } else if (settling_proof != QL_COMBINE_NO_INPUT) {
            created->verdict = inputs[settling_proof].outcome.verdict;
            created->deciding_input = settling_proof;
        } else if (created->refinement_composition_applied != 0u) {
            created->verdict = QL_VERDICT_PROVED_EQUIVALENT;
            created->deciding_input = left_refinement;
            created->supporting_input = right_refinement;
        } else if (counterexample != QL_COMBINE_NO_INPUT) {
            created->verdict = QL_VERDICT_COUNTEREXAMPLE;
            created->deciding_input = counterexample;
        } else if (created->bounded_count != 0u) {
            /* Rule 6. Any number of bounded results remains bounded. */
            created->verdict = QL_VERDICT_BOUNDED_CLEAN;
        } else {
            /* Rule 5. Several UNKNOWN results remain UNKNOWN, and a proof of
               a relation weaker than the one asked for does not fill in. */
            created->verdict = QL_VERDICT_UNKNOWN;
        }
    }

    bound_total = count_bounds(inputs, input_count, effective);
    if (bound_total != 0u) {
        created->bounds = actual->allocate(
            actual->user_data, bound_total * sizeof(*created->bounds));
        if (created->bounds == NULL) {
            actual->deallocate(actual->user_data, effective);
            destroy_result(created);
            ql_error_set(error, QL_STATUS_OUT_OF_MEMORY,
                         "could not allocate the retained bound vector");
            return QL_STATUS_OUT_OF_MEMORY;
        }
        for (index = 0u; index < input_count; ++index) {
            if (effective[index] != QL_VERDICT_BOUNDED_CLEAN ||
                inputs[index].bound_count == 0u) {
                continue;
            }
            /* Copied whole and never merged across inputs: two bounds on
               different dimensions describe different covered state sets. */
            memcpy(&created->bounds[bound_offset], inputs[index].bounds,
                   inputs[index].bound_count * sizeof(*created->bounds));
            bound_offset += inputs[index].bound_count;
        }
        created->bound_count = bound_total;
    }

    actual->deallocate(actual->user_data, effective);
    *result = created;
    ql_error_clear(error);
    return QL_STATUS_OK;
}

void QL_CALL ql_combine_result_release(ql_combine_result *result) {
    destroy_result(result);
}

ql_status QL_CALL ql_combine_result_get_view(
    const ql_combine_result *result, ql_combine_result_view_v1 *view,
    ql_error *error) {
    if (result == NULL || view == NULL) {
        ql_error_set(error, QL_STATUS_INVALID_ARGUMENT,
                     "a combine result and a view are required");
        return QL_STATUS_INVALID_ARGUMENT;
    }
    memset(view, 0, sizeof(*view));
    view->struct_size = sizeof(*view);
    view->schema_version = QL_COMBINE_SCHEMA_VERSION;
    view->inconsistent = result->inconsistent;
    view->verdict = result->verdict;
    view->refinement_composition_applied =
        result->refinement_composition_applied;
    view->deciding_input = result->deciding_input;
    view->supporting_input = result->supporting_input;
    view->conflicting_input = result->conflicting_input;
    view->accepted_count = result->accepted_count;
    view->withdrawn_count = result->withdrawn_count;
    view->bounded_count = result->bounded_count;
    view->findings = result->findings;
    view->finding_count = result->finding_count;
    view->bounds = result->bounds;
    view->bound_count = result->bound_count;
    ql_error_clear(error);
    return QL_STATUS_OK;
}

const char *QL_CALL ql_combine_disposition_string(
    ql_combine_disposition disposition) {
    switch (disposition) {
    case QL_COMBINE_DISPOSITION_ACCEPTED:
        return "accepted";
    case QL_COMBINE_DISPOSITION_WITHDRAWN:
        return "withdrawn";
    case QL_COMBINE_DISPOSITION_NEUTRAL:
        return "neutral";
    case QL_COMBINE_DISPOSITION_BOUNDED:
        return "bounded";
    default:
        return "invalid";
    }
}

const char *QL_CALL ql_combine_code_string(ql_combine_code code) {
    switch (code) {
    case QL_COMBINE_CODE_BUDGET_EXHAUSTED:
        return "budget-exhausted";
    case QL_COMBINE_CODE_UNCHECKED_PROOF:
        return "unchecked-proof";
    case QL_COMBINE_CODE_UNTRUSTED_BACKEND:
        return "untrusted-backend";
    case QL_COMBINE_CODE_UNREPLAYED_WITNESS:
        return "unreplayed-witness";
    case QL_COMBINE_CODE_BROKEN_NORMALIZATION:
        return "broken-normalization";
    case QL_COMBINE_CODE_WEAKER_RELATION:
        return "weaker-relation";
    default:
        return "none";
    }
}
