#include "quodlibet/registry.h"

#include "quodlibet/proof_aigsat.h"
#include "quodlibet/proof_diff.h"
#include "quodlibet/proof_smt.h"

static ql_status QL_CALL identity_run(
    void *instance, const ql_run_context_v1 *context,
    ql_artifact *const *inputs, size_t input_count, ql_artifact **output,
    ql_error *error) {
    (void)instance;

    if (context == NULL || context->host == NULL || output == NULL ||
        inputs == NULL || input_count != 1u || inputs[0] == NULL) {
        ql_error_set(error, QL_STATUS_INVALID_ARGUMENT,
                     "builtin.identity requires exactly one artifact");
        return QL_STATUS_INVALID_ARGUMENT;
    }
    context->host->artifact_retain(inputs[0]);
    *output = inputs[0];
    ql_error_clear(error);
    return QL_STATUS_OK;
}

static const ql_method_v1 identity_method = {
    sizeof(ql_method_v1),
    QL_ABI_VERSION,
    "builtin.identity",
    "Pass one immutable artifact through unchanged",
    NULL,
    QL_METHOD_DETERMINISTIC | QL_METHOD_CACHEABLE,
    1u,
    1u,
    NULL,
    NULL,
    identity_run,
    NULL,
    { NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL }
};

ql_status QL_CALL ql_register_builtin_methods(ql_registry *registry,
                                              ql_error *error) {
    ql_status status;

    status = ql_registry_register(registry, &identity_method, error);
    if (status != QL_STATUS_OK) {
        return status;
    }
    /* The SMT product-program prover is a built-in method, not a privileged
       one: it still passes the same capability checks as any plugin. */
    status = ql_register_smt_product_method(registry, error);
    if (status != QL_STATUS_OK) {
        return status;
    }
    /* The differential refuter is registered beside it rather than under it.
       It proves nothing and needs no solver, so a pipeline can run both
       against one problem and take whichever answers first. */
    status = ql_register_diff_method(registry, error);
    if (status != QL_STATUS_OK) {
        return status;
    }
    /* The AIG/SAT prover answers the same question as the SMT one and trusts
       less to do it: its UNSAT is promoted only after an independent checker
       verifies the certificate. It is registered beside both so a pipeline can
       choose how much trust it is willing to spend. */
    return ql_aig_sat_register_method(registry, error);
}
