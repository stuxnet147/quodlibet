#ifndef QUODLIBET_PYTHON_QL_CHECK_H
#define QUODLIBET_PYTHON_QL_CHECK_H

/* The whole judgement, expressed without a single CPython symbol.

   The extension module copies its arguments into ql_py_spec while it holds the
   GIL, releases the GIL, calls ql_py_check, and only then reads ql_py_result.
   Keeping this layer Python-free is what makes that safe: there is no way for
   a call made without the GIL to touch an interpreter object. */

#include "quodlibet/budget.h"
#include "quodlibet/policy.h"
#include "quodlibet/proof_smt.h"
#include "quodlibet/semantics.h"
#include "quodlibet/status.h"

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define QL_PY_DIAGNOSTIC_CAPACITY 512u

typedef struct ql_py_argument_binding {
    uint32_t left_index;
    uint32_t right_index;
} ql_py_argument_binding;

/* Every char pointer is owned by the spec and freed by ql_py_spec_dispose.
   Sources are held as explicit sizes because C sources may contain no
   terminator assumptions beyond the copy this layer makes. */
typedef struct ql_py_spec {
    char *left_source;
    size_t left_source_size;
    char *left_function;
    char *right_source;
    size_t right_source_size;
    char *right_function;

    ql_relation relation;
    ql_ub_policy ub_policy;
    uint64_t observations;
    ql_memory_observation memory_observation;
    ql_external_call_observation external_call_observation;
    char *precondition_json;
    size_t precondition_json_size;

    ql_py_argument_binding *bindings;
    size_t binding_count;

    uint32_t trust_smt_backend;
    uint64_t solver_timeout_ms;
    uint64_t solver_memory_limit_mb;
    char *solver_executable;
    /* Borrowed ql_solver_session*, or null to establish one for this check
       alone. Not owned by the spec and not freed with it: the caller keeps a
       session alive across many checks, which is the whole reason it exists.
       A session is not thread safe, so the caller gives each worker its
       own. */
    void *solver_session;

    ql_budget_limits_v1 limits;

    char *policy_json;
    size_t policy_json_size;
} ql_py_spec;

/* Why a run produced no verdict of its own. */
typedef enum ql_py_outcome_kind {
    /* The method ran and the outcome envelope below is real. */
    QL_PY_OUTCOME_METHOD = 0,
    /* A source is outside the restricted-C slice. UNKNOWN, not an error. */
    QL_PY_OUTCOME_UNSUPPORTED = 1,
    /* A budget axis was exhausted. UNKNOWN, not an error. */
    QL_PY_OUTCOME_BUDGET = 2
} ql_py_outcome_kind;

typedef struct ql_py_result {
    /* Zero means the call failed before any verdict existed; `status` and
       `message` then describe the failure and the caller raises. */
    uint32_t ok;
    ql_status status;
    char message[QL_ERROR_MESSAGE_CAPACITY];

    ql_py_outcome_kind outcome_kind;
    ql_verdict verdict;
    ql_evidence_class evidence_class;
    ql_unsat_promotion_policy unsat_promotion;
    ql_smt_product_answer violation_answer;
    ql_smt_product_answer domain_answer;
    uint32_t checked_proof;
    uint32_t replay_confirmed;
    uint32_t budget_exhausted;
    ql_budget_state budget_state;
    char diagnostic[QL_PY_DIAGNOSTIC_CAPACITY];

    char problem_digest[QL_DIGEST_HEX_SIZE];
    char solver_query_digest[QL_DIGEST_HEX_SIZE];
    char solver_binary_digest[QL_DIGEST_HEX_SIZE];
    char cache_key[QL_DIGEST_HEX_SIZE];
    char counterexample_digest[QL_DIGEST_HEX_SIZE];

    ql_loop_proof_stats_v1 loop_proof;

    /* Owned canonical counterexample bytes, or null. */
    char *counterexample_json;
    size_t counterexample_json_size;

    uint32_t has_policy;
    ql_policy_result_v1 policy;

    ql_budget_usage_v1 usage;
} ql_py_result;

void ql_py_spec_init(ql_py_spec *spec);
void ql_py_spec_dispose(ql_py_spec *spec);
void ql_py_result_init(ql_py_result *result);
void ql_py_result_dispose(ql_py_result *result);

/* Never fails by any route other than ql_py_result.ok, so the caller has one
   place to look. Releases every core handle it opens before returning. */
void ql_py_check(const ql_py_spec *spec, ql_py_result *result);

/* Duplicates `size` bytes plus a terminator. Null on allocation failure. */
char *ql_py_strdup(const char *text, size_t size);

/* Whether this build can reach a Bitwuzla executable at all. */
uint32_t ql_py_backend_available(void);
const char *ql_py_backend_path(void);

#ifdef __cplusplus
}
#endif

#endif
