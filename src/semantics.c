#include "quodlibet/semantics.h"

#include <string.h>

#include "yyjson.h"

static int valid_relation(ql_relation relation) {
    return relation == QL_RELATION_EQUIVALENCE ||
           relation == QL_RELATION_LEFT_REFINES_RIGHT ||
           relation == QL_RELATION_RIGHT_REFINES_LEFT;
}

static int valid_ub_policy(ql_ub_policy policy) {
    return policy == QL_UB_MUST_MATCH ||
           policy == QL_UB_LANGUAGE_REFINEMENT ||
           policy == QL_UB_COMPARE_WHERE_BOTH_DEFINED;
}

static ql_status validate_precondition(const ql_semantic_contract_v1 *contract,
                                       ql_error *error) {
    yyjson_doc *document;
    yyjson_val *root;
    yyjson_val *schema_version;
    yyjson_val *expression;
    yyjson_read_err json_error;

    if (contract->precondition_json == NULL) {
        if (contract->precondition_json_size != 0u) {
            ql_error_set(error, QL_STATUS_INVALID_ARGUMENT,
                         "precondition size requires JSON bytes");
            return QL_STATUS_INVALID_ARGUMENT;
        }
        return QL_STATUS_OK;
    }
    if (contract->precondition_json_size == 0u) {
        ql_error_set(error, QL_STATUS_INVALID_ARGUMENT,
                     "precondition JSON cannot be empty");
        return QL_STATUS_INVALID_ARGUMENT;
    }

    document = yyjson_read_opts((char *)contract->precondition_json,
                                contract->precondition_json_size, 0u, NULL,
                                &json_error);
    if (document == NULL) {
        ql_error_set(error, QL_STATUS_PARSE_ERROR,
                     "invalid precondition JSON at byte %zu: %s",
                     json_error.pos,
                     json_error.msg != NULL ? json_error.msg : "parse error");
        return QL_STATUS_PARSE_ERROR;
    }
    root = yyjson_doc_get_root(document);
    schema_version = yyjson_obj_get(root, "schema_version");
    expression = yyjson_obj_get(root, "expression");
    if (!yyjson_is_obj(root) || !yyjson_is_uint(schema_version) ||
        yyjson_get_uint(schema_version) != QL_PRECONDITION_SCHEMA_VERSION ||
        expression == NULL ||
        !(yyjson_is_obj(expression) || yyjson_is_bool(expression))) {
        yyjson_doc_free(document);
        ql_error_set(error, QL_STATUS_SCHEMA_MISMATCH,
                     "precondition must be schema version 1 with an object or boolean expression");
        return QL_STATUS_SCHEMA_MISMATCH;
    }
    yyjson_doc_free(document);
    return QL_STATUS_OK;
}

void QL_CALL ql_semantic_contract_init(ql_semantic_contract_v1 *contract) {
    if (contract == NULL) {
        return;
    }
    memset(contract, 0, sizeof(*contract));
    contract->struct_size = sizeof(*contract);
    contract->schema_version = QL_SEMANTIC_CONTRACT_SCHEMA_VERSION;
    contract->relation = QL_RELATION_EQUIVALENCE;
    contract->ub_policy = QL_UB_MUST_MATCH;
    contract->observations = QL_OBSERVE_ALL;
    contract->memory_observation = QL_MEMORY_FINAL_REACHABLE_STATE;
    contract->external_call_observation = QL_EXTERNAL_CALLS_ORDERED_TRACE;
    contract->c_dialect = QL_C_DIALECT_ASM2C_GNU_V1;
    contract->target_abi = QL_TARGET_ABI_X86_64_LINUX_SYSV_LP64;
    contract->compiler_families =
        QL_COMPILER_FAMILY_GCC | QL_COMPILER_FAMILY_CLANG;
    contract->codegen_modes = QL_CODEGEN_PIC | QL_CODEGEN_NON_PIC;
    contract->target_features = QL_TARGET_FEATURE_X86_64_BASE;
}

ql_status QL_CALL ql_semantic_contract_validate(
    const ql_semantic_contract_v1 *contract, ql_error *error) {
    const uint64_t known_observations = QL_OBSERVE_ALL;
    const uint32_t known_compilers =
        QL_COMPILER_FAMILY_GCC | QL_COMPILER_FAMILY_CLANG;
    const uint32_t known_codegen_modes = QL_CODEGEN_PIC | QL_CODEGEN_NON_PIC;
    const uint64_t known_features =
        QL_TARGET_FEATURE_X86_64_BASE | QL_TARGET_FEATURE_DOMAIN_BASELINE |
        QL_TARGET_FEATURE_AVX512 | QL_TARGET_FEATURE_CRYPTO |
        QL_TARGET_FEATURE_BITMANIP | QL_TARGET_FEATURE_RANDOM |
        QL_TARGET_FEATURE_TSX | QL_TARGET_FEATURE_TIMING;
    ql_status status;

    if (contract == NULL || contract->struct_size < sizeof(*contract)) {
        ql_error_set(error, QL_STATUS_INVALID_ARGUMENT,
                     "semantic contract v1 has an invalid size");
        return QL_STATUS_INVALID_ARGUMENT;
    }
    if (contract->schema_version != QL_SEMANTIC_CONTRACT_SCHEMA_VERSION) {
        ql_error_set(error, QL_STATUS_SCHEMA_MISMATCH,
                     "unsupported semantic contract schema version %u",
                     contract->schema_version);
        return QL_STATUS_SCHEMA_MISMATCH;
    }
    if (!valid_relation(contract->relation) ||
        !valid_ub_policy(contract->ub_policy)) {
        ql_error_set(error, QL_STATUS_INVALID_ARGUMENT,
                     "semantic relation or UB policy is invalid");
        return QL_STATUS_INVALID_ARGUMENT;
    }
    if ((contract->observations & ~known_observations) != 0u) {
        ql_error_set(error, QL_STATUS_INVALID_ARGUMENT,
                     "semantic contract has unknown observation bits");
        return QL_STATUS_INVALID_ARGUMENT;
    }
    if (((contract->observations & QL_OBSERVE_MEMORY) == 0u) !=
        (contract->memory_observation == QL_MEMORY_IGNORE)) {
        ql_error_set(error, QL_STATUS_INVALID_ARGUMENT,
                     "memory observation bit and mode disagree");
        return QL_STATUS_INVALID_ARGUMENT;
    }
    if (contract->memory_observation < QL_MEMORY_IGNORE ||
        contract->memory_observation > QL_MEMORY_FULL_TRACE) {
        ql_error_set(error, QL_STATUS_INVALID_ARGUMENT,
                     "memory observation mode is invalid");
        return QL_STATUS_INVALID_ARGUMENT;
    }
    if (((contract->observations & QL_OBSERVE_EXTERNAL_CALLS) == 0u) !=
        (contract->external_call_observation == QL_EXTERNAL_CALLS_IGNORE)) {
        ql_error_set(error, QL_STATUS_INVALID_ARGUMENT,
                     "external-call observation bit and mode disagree");
        return QL_STATUS_INVALID_ARGUMENT;
    }
    if (contract->external_call_observation < QL_EXTERNAL_CALLS_IGNORE ||
        contract->external_call_observation > QL_EXTERNAL_CALLS_ORDERED_TRACE) {
        ql_error_set(error, QL_STATUS_INVALID_ARGUMENT,
                     "external-call observation mode is invalid");
        return QL_STATUS_INVALID_ARGUMENT;
    }
    if (contract->c_dialect != QL_C_DIALECT_ASM2C_GNU_V1 ||
        contract->target_abi != QL_TARGET_ABI_X86_64_LINUX_SYSV_LP64) {
        ql_error_set(error, QL_STATUS_INVALID_ARGUMENT,
                     "unsupported C dialect or target ABI profile");
        return QL_STATUS_INVALID_ARGUMENT;
    }
    if (contract->compiler_families == 0u ||
        (contract->compiler_families & ~known_compilers) != 0u ||
        contract->codegen_modes == 0u ||
        (contract->codegen_modes & ~known_codegen_modes) != 0u ||
        (contract->target_features & QL_TARGET_FEATURE_X86_64_BASE) == 0u ||
        (contract->target_features & ~known_features) != 0u) {
        ql_error_set(error, QL_STATUS_INVALID_ARGUMENT,
                     "compiler, codegen, or target-feature profile is invalid");
        return QL_STATUS_INVALID_ARGUMENT;
    }
    if ((contract->target_features &
         (QL_TARGET_FEATURE_AVX512 | QL_TARGET_FEATURE_CRYPTO |
          QL_TARGET_FEATURE_BITMANIP | QL_TARGET_FEATURE_RANDOM |
          QL_TARGET_FEATURE_TSX | QL_TARGET_FEATURE_TIMING)) != 0u &&
        (contract->target_features & QL_TARGET_FEATURE_DOMAIN_BASELINE) == 0u) {
        ql_error_set(error, QL_STATUS_INVALID_ARGUMENT,
                     "domain feature profile requires the domain baseline");
        return QL_STATUS_INVALID_ARGUMENT;
    }
    status = validate_precondition(contract, error);
    if (status != QL_STATUS_OK) {
        return status;
    }
    ql_error_clear(error);
    return QL_STATUS_OK;
}

const char *QL_CALL ql_verdict_string(ql_verdict verdict) {
    switch (verdict) {
    case QL_VERDICT_UNKNOWN: return "UNKNOWN";
    case QL_VERDICT_PROVED_EQUIVALENT: return "PROVED_EQUIVALENT";
    case QL_VERDICT_PROVED_LEFT_REFINES_RIGHT:
        return "PROVED_LEFT_REFINES_RIGHT";
    case QL_VERDICT_PROVED_RIGHT_REFINES_LEFT:
        return "PROVED_RIGHT_REFINES_LEFT";
    case QL_VERDICT_COUNTEREXAMPLE: return "COUNTEREXAMPLE";
    case QL_VERDICT_BOUNDED_CLEAN: return "BOUNDED_CLEAN";
    default: return "INVALID_VERDICT";
    }
}
