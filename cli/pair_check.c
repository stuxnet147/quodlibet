/* quodlibet-pair-check: run one method on one C pair and print its verdict.
 *
 * The CLI otherwise has no direct pair-check command; end-to-end judgements
 * are reached only through the Python binding, and that binding runs the fixed
 * prove.smt-product chain. This tool exposes any single registered method
 * (prove.smt-product, prove.egraph, prove.aig-sat, refute.concrete-differential,
 * search.bounded-symbolic, prove.chc-pdr) on a pair, so a method can be measured
 * on its own rather than only as a follow-up.
 *
 * Usage:
 *   quodlibet-pair-check <left.c> <right.c> <function> <method>
 *                        [smt_solver] [sat_solver] [lrat_checker]
 *
 * smt_solver feeds the SMT-backed methods (smt-product, bounded, chc-pdr);
 * sat_solver and lrat_checker feed aig-sat. Prints one line:
 *   OK <verdict_name> | UNSUPPORTED <side> | ERROR <stage> <message>
 *
 * The parse -> lower -> signature -> problem-artifact -> pipeline path mirrors
 * the binding's ql_py_check; only the method registered and the outcome reader
 * vary by name.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "quodlibet/budget.h"
#include "quodlibet/c_frontend.h"
#include "quodlibet/c_lower.h"
#include "quodlibet/pipeline.h"
#include "quodlibet/policy.h"
#include "quodlibet/problem.h"
#include "quodlibet/proof_aigsat.h"
#include "quodlibet/proof_bounded.h"
#include "quodlibet/proof_chcpdr.h"
#include "quodlibet/proof_diff.h"
#include "quodlibet/proof_egraph.h"
#include "quodlibet/proof_smt.h"
#include "quodlibet/registry.h"
#include "quodlibet/scheduler.h"
#include "quodlibet/semantics.h"
#include "quodlibet/signature.h"

static char *slurp(const char *path, size_t *len) {
    FILE *file = fopen(path, "rb");
    if (file == NULL) {
        return NULL;
    }
    fseek(file, 0, SEEK_END);
    long size = ftell(file);
    fseek(file, 0, SEEK_SET);
    if (size < 0) {
        fclose(file);
        return NULL;
    }
    char *buffer = malloc((size_t)size + 1u);
    if (buffer == NULL) {
        fclose(file);
        return NULL;
    }
    size_t got = fread(buffer, 1u, (size_t)size, file);
    buffer[got] = '\0';
    *len = got;
    fclose(file);
    return buffer;
}

typedef struct {
    ql_c_frontend_unit *unit;
    ql_c_lower_result *lowered;
    ql_artifact *signature_artifact;
    const ql_artifact *ir_artifact;
    ql_c_lower_support support;
} pair_side;

static ql_status side_build(pair_side *side, const ql_allocator *allocator,
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
                                          side->unit, &function, &side->lowered,
                                          error);
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
        return QL_STATUS_OK;
    }
    return ql_source_signature_from_c_function_v2(
        allocator, side->unit, &function, source, source_size,
        QL_C_DIALECT_ASM2C_GNU_V1, QL_TARGET_ABI_X86_64_LINUX_SYSV_LP64,
        &side->signature_artifact, error);
}

static ql_status register_method(ql_registry *registry, const char *method,
                                  const char **name, ql_error *error) {
    if (strcmp(method, "smt-product") == 0) {
        *name = QL_SMT_PRODUCT_METHOD_NAME;
        return ql_register_smt_product_method(registry, error);
    }
    if (strcmp(method, "egraph") == 0) {
        *name = QL_EGRAPH_PROOF_METHOD_NAME;
        return ql_register_egraph_proof_method(registry, error);
    }
    if (strcmp(method, "aig-sat") == 0) {
        *name = QL_AIG_SAT_METHOD_NAME;
        return ql_aig_sat_register_method(registry, error);
    }
    if (strcmp(method, "diff") == 0) {
        *name = QL_DIFF_METHOD_NAME;
        return ql_register_diff_method(registry, error);
    }
    if (strcmp(method, "bounded") == 0) {
        *name = QL_BOUNDED_METHOD_NAME;
        return ql_register_bounded_method(registry, error);
    }
    if (strcmp(method, "chc-pdr") == 0) {
        *name = QL_CHCPDR_METHOD_NAME;
        return ql_register_chcpdr_method(registry, error);
    }
    return QL_STATUS_INVALID_ARGUMENT;
}

static int read_verdict(const char *method, const ql_artifact *artifact,
                        ql_verdict *verdict, ql_error *error) {
    if (strcmp(method, "egraph") == 0) {
        ql_egraph_proof_outcome_view_v1 outcome;
        memset(&outcome, 0, sizeof(outcome));
        outcome.struct_size = sizeof(outcome);
        if (ql_egraph_proof_outcome_read(artifact, &outcome, error) != QL_STATUS_OK) {
            return -1;
        }
        *verdict = outcome.verdict;
        return 0;
    }
    if (strcmp(method, "aig-sat") == 0) {
        ql_aig_sat_outcome_view_v1 outcome;
        memset(&outcome, 0, sizeof(outcome));
        outcome.struct_size = sizeof(outcome);
        if (ql_aig_sat_outcome_read(artifact, &outcome, error) != QL_STATUS_OK) {
            return -1;
        }
        *verdict = outcome.verdict;
        return 0;
    }
    if (strcmp(method, "diff") == 0) {
        ql_diff_outcome_view_v1 outcome;
        memset(&outcome, 0, sizeof(outcome));
        outcome.struct_size = sizeof(outcome);
        if (ql_diff_outcome_read(artifact, &outcome, error) != QL_STATUS_OK) {
            return -1;
        }
        *verdict = outcome.verdict;
        return 0;
    }
    if (strcmp(method, "smt-product") == 0) {
        ql_smt_product_outcome_view_v1 outcome;
        memset(&outcome, 0, sizeof(outcome));
        outcome.struct_size = sizeof(outcome);
        if (ql_smt_product_outcome_read(artifact, &outcome, error) != QL_STATUS_OK) {
            return -1;
        }
        *verdict = outcome.verdict;
        return 0;
    }
    if (strcmp(method, "bounded") == 0) {
        ql_bounded_outcome_view_v1 outcome;
        memset(&outcome, 0, sizeof(outcome));
        outcome.struct_size = sizeof(outcome);
        if (ql_bounded_outcome_read(artifact, &outcome, error) != QL_STATUS_OK) {
            return -1;
        }
        *verdict = outcome.verdict;
        return 0;
    }
    if (strcmp(method, "chc-pdr") == 0) {
        ql_chcpdr_outcome_view_v1 outcome;
        memset(&outcome, 0, sizeof(outcome));
        outcome.struct_size = sizeof(outcome);
        if (ql_chcpdr_outcome_read(artifact, &outcome, error) != QL_STATUS_OK) {
            return -1;
        }
        *verdict = outcome.verdict;
        return 0;
    }
    return -1;
}

static void build_options(const char *method, const char *smt_solver,
                          const char *sat_solver, const char *lrat_checker,
                          char *buffer, size_t capacity) {
    if (strcmp(method, "smt-product") == 0) {
        snprintf(buffer, capacity,
                 "{\"unsat_promotion\":\"trusted-backend\",\"timeout_ms\":5000,"
                 "\"memory_limit_mb\":0,\"solver_options\":\"{\\\"executable\\\":"
                 "\\\"%s\\\"}\"}",
                 smt_solver != NULL ? smt_solver : "");
    } else if (strcmp(method, "bounded") == 0) {
        snprintf(buffer, capacity,
                 "{\"unroll_bound\":8,\"timeout_ms\":5000,\"memory_limit_mb\":0,"
                 "\"solver_options\":\"{\\\"executable\\\":\\\"%s\\\"}\"}",
                 smt_solver != NULL ? smt_solver : "");
    } else if (strcmp(method, "chc-pdr") == 0) {
        snprintf(buffer, capacity,
                 "{\"unsat_promotion\":\"trusted-backend\",\"timeout_ms\":5000,"
                 "\"solver_options\":\"{\\\"executable\\\":\\\"%s\\\"}\"}",
                 smt_solver != NULL ? smt_solver : "");
    } else if (strcmp(method, "aig-sat") == 0) {
        snprintf(buffer, capacity,
                 "{\"timeout_ms\":5000,\"memory_limit_mb\":0,\"solver_executable\":"
                 "\"%s\",\"checker_executable\":\"%s\"}",
                 sat_solver != NULL ? sat_solver : "",
                 lrat_checker != NULL ? lrat_checker : "");
    } else {
        snprintf(buffer, capacity, "{}");
    }
}

int main(int argc, char **argv) {
    if (argc < 5) {
        fprintf(stderr,
                "usage: %s <left.c> <right.c> <function> <method> "
                "[smt_solver] [sat_solver] [lrat_checker]\n"
                "method: smt-product egraph aig-sat diff bounded chc-pdr\n",
                argv[0]);
        return 2;
    }
    const char *left_path = argv[1];
    const char *right_path = argv[2];
    const char *function = argv[3];
    const char *method = argv[4];
    const char *smt_solver = argc > 5 ? argv[5] : NULL;
    const char *sat_solver = argc > 6 ? argv[6] : NULL;
    const char *lrat_checker = argc > 7 ? argv[7] : NULL;

    ql_error error;
    ql_error_clear(&error);
    size_t left_size = 0u;
    size_t right_size = 0u;
    char *left = slurp(left_path, &left_size);
    char *right = slurp(right_path, &right_size);
    if (left == NULL || right == NULL) {
        printf("ERROR slurp could not read a source\n");
        return 1;
    }

    ql_budget_limits_v1 limits;
    ql_budget_limits_init(&limits);
    limits.total_wall_clock_ns = 20000000000ULL;
    limits.solver_wall_clock_ns = 5000000000ULL;
    ql_budget *budget = NULL;
    if (ql_budget_create(NULL, &limits, &budget, &error) != QL_STATUS_OK) {
        printf("ERROR budget %s\n", error.message);
        return 1;
    }
    const ql_allocator *allocator = ql_budget_allocator(budget);

    pair_side left_side;
    pair_side right_side;
    if (side_build(&left_side, allocator, left, left_size, function, &error) !=
        QL_STATUS_OK) {
        printf("ERROR left_build %s\n", error.message);
        return 1;
    }
    if (side_build(&right_side, allocator, right, right_size, function, &error) !=
        QL_STATUS_OK) {
        printf("ERROR right_build %s\n", error.message);
        return 1;
    }
    if (left_side.support != QL_C_LOWER_SUPPORTED) {
        printf("UNSUPPORTED left\n");
        return 0;
    }
    if (right_side.support != QL_C_LOWER_SUPPORTED) {
        printf("UNSUPPORTED right\n");
        return 0;
    }

    ql_source_signature *signature = NULL;
    ql_source_signature_view_v1 signature_view;
    memset(&signature_view, 0, sizeof(signature_view));
    signature_view.struct_size = sizeof(signature_view);
    size_t binding_count = 0u;
    if (ql_source_signature_open(allocator, left_side.signature_artifact,
                                 &signature, &error) == QL_STATUS_OK &&
        ql_source_signature_get_view(signature, &signature_view, &error) ==
            QL_STATUS_OK) {
        binding_count = signature_view.argument_count;
    }
    ql_source_signature_release(signature);

    ql_problem_argument_binding_v1 *bindings = NULL;
    if (binding_count > 0u) {
        bindings = calloc(binding_count, sizeof(*bindings));
        if (bindings == NULL) {
            printf("ERROR bindings out of memory\n");
            return 1;
        }
        for (size_t index = 0u; index < binding_count; ++index) {
            bindings[index].struct_size = sizeof(bindings[index]);
            bindings[index].left_index = (uint32_t)index;
            bindings[index].right_index = (uint32_t)index;
        }
    }

    ql_semantic_contract_v1 contract;
    ql_semantic_contract_init(&contract);
    contract.relation = QL_RELATION_EQUIVALENCE;
    contract.ub_policy = QL_UB_MUST_MATCH;
    contract.observations =
        QL_OBSERVE_RETURN_VALUE | QL_OBSERVE_MEMORY | QL_OBSERVE_EXTERNAL_CALLS;
    contract.memory_observation = QL_MEMORY_FINAL_REACHABLE_STATE;
    contract.external_call_observation = QL_EXTERNAL_CALLS_ORDERED_TRACE;

    ql_problem_definition_v2 definition;
    ql_problem_definition_v2_init(&definition);
    definition.contract = contract;
    definition.left_source = left;
    definition.left_source_size = left_size;
    definition.left_function_name = function;
    definition.left_function_name_size = strlen(function);
    definition.right_source = right;
    definition.right_source_size = right_size;
    definition.right_function_name = function;
    definition.right_function_name_size = strlen(function);
    definition.left_signature = left_side.signature_artifact;
    definition.right_signature = right_side.signature_artifact;
    definition.argument_bindings = bindings;
    definition.argument_binding_count = binding_count;

    ql_artifact *problem = NULL;
    if (ql_problem_artifact_create_v2(allocator, &definition, &problem, &error) !=
        QL_STATUS_OK) {
        printf("ERROR problem %s\n", error.message);
        return 1;
    }

    ql_registry *registry = NULL;
    if (ql_registry_create(allocator, &registry, &error) != QL_STATUS_OK) {
        printf("ERROR registry %s\n", error.message);
        return 1;
    }
    const char *method_name = NULL;
    if (register_method(registry, method, &method_name, &error) != QL_STATUS_OK) {
        printf("ERROR register %s\n", error.message);
        return 1;
    }

    char options[2600];
    build_options(method, smt_solver, sat_solver, lrat_checker, options,
                  sizeof(options));

    ql_pipeline *pipeline = NULL;
    ql_node_id node_id = QL_INVALID_NODE_ID;
    if (ql_pipeline_create(registry, allocator, &pipeline, &error) != QL_STATUS_OK) {
        printf("ERROR pipeline %s\n", error.message);
        return 1;
    }
    if (ql_pipeline_add_node(pipeline, "check", method_name, options, NULL, 0u,
                             &node_id, &error) != QL_STATUS_OK) {
        printf("ERROR add_node %s\n", error.message);
        return 1;
    }
    if (ql_pipeline_compile(pipeline, &error) != QL_STATUS_OK) {
        printf("ERROR compile %s\n", error.message);
        return 1;
    }
    ql_scheduler *scheduler = NULL;
    if (ql_scheduler_create(allocator, 1u, &scheduler, &error) != QL_STATUS_OK) {
        printf("ERROR scheduler %s\n", error.message);
        return 1;
    }
    ql_budget_start(budget);
    ql_pipeline_result *run_result = NULL;
    if (ql_pipeline_run_with_budget(pipeline, scheduler, problem, NULL, budget,
                                    &run_result, &error) != QL_STATUS_OK) {
        printf("ERROR run %s\n", error.message);
        return 1;
    }
    if (ql_pipeline_result_count(run_result) != 1u) {
        printf("ERROR no_outcome\n");
        return 1;
    }

    ql_verdict verdict;
    if (read_verdict(method, ql_pipeline_result_artifact(run_result, 0u),
                     &verdict, &error) != 0) {
        printf("ERROR read %s\n", error.message);
        return 1;
    }
    printf("OK %s\n", ql_policy_verdict_name(verdict));
    return 0;
}
