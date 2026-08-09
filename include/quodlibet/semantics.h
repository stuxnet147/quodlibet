#ifndef QUODLIBET_SEMANTICS_H
#define QUODLIBET_SEMANTICS_H

#include "quodlibet/common.h"
#include "quodlibet/hash.h"
#include "quodlibet/status.h"

QL_EXTERN_C_BEGIN

typedef enum ql_relation {
    QL_RELATION_EQUIVALENCE = 0,
    QL_RELATION_LEFT_REFINES_RIGHT,
    QL_RELATION_RIGHT_REFINES_LEFT
} ql_relation;

typedef enum ql_ub_policy {
    QL_UB_MUST_MATCH = 0,
    QL_UB_LANGUAGE_REFINEMENT,
    QL_UB_COMPARE_WHERE_BOTH_DEFINED
} ql_ub_policy;

typedef enum ql_observation {
    QL_OBSERVE_RETURN_VALUE = UINT64_C(1) << 0,
    QL_OBSERVE_MEMORY = UINT64_C(1) << 1,
    QL_OBSERVE_TERMINATION = UINT64_C(1) << 2,
    QL_OBSERVE_VOLATILE = UINT64_C(1) << 3,
    QL_OBSERVE_ATOMICS = UINT64_C(1) << 4,
    QL_OBSERVE_IO = UINT64_C(1) << 5,
    QL_OBSERVE_TRAPS = UINT64_C(1) << 6,
    QL_OBSERVE_UNDEFINED_BEHAVIOR = UINT64_C(1) << 7,
    QL_OBSERVE_EXTERNAL_CALLS = UINT64_C(1) << 8
} ql_observation;

#define QL_OBSERVE_ALL                                                     \
    (QL_OBSERVE_RETURN_VALUE | QL_OBSERVE_MEMORY |                         \
     QL_OBSERVE_TERMINATION | QL_OBSERVE_VOLATILE | QL_OBSERVE_ATOMICS |  \
     QL_OBSERVE_IO | QL_OBSERVE_TRAPS |                                    \
     QL_OBSERVE_UNDEFINED_BEHAVIOR | QL_OBSERVE_EXTERNAL_CALLS)

typedef enum ql_memory_observation {
    QL_MEMORY_IGNORE = 0,
    QL_MEMORY_FINAL_REACHABLE_STATE,
    QL_MEMORY_ORDERED_WRITES,
    QL_MEMORY_FULL_TRACE
} ql_memory_observation;

typedef enum ql_external_call_observation {
    QL_EXTERNAL_CALLS_IGNORE = 0,
    QL_EXTERNAL_CALLS_ORDERED_TRACE
} ql_external_call_observation;

typedef enum ql_c_dialect_profile {
    QL_C_DIALECT_INVALID = 0,
    QL_C_DIALECT_ASM2C_GNU_V1
} ql_c_dialect_profile;

typedef enum ql_target_abi {
    QL_TARGET_ABI_INVALID = 0,
    QL_TARGET_ABI_X86_64_LINUX_SYSV_LP64
} ql_target_abi;

typedef enum ql_compiler_family {
    QL_COMPILER_FAMILY_GCC = UINT32_C(1) << 0,
    QL_COMPILER_FAMILY_CLANG = UINT32_C(1) << 1
} ql_compiler_family;

typedef enum ql_codegen_mode {
    QL_CODEGEN_PIC = UINT32_C(1) << 0,
    QL_CODEGEN_NON_PIC = UINT32_C(1) << 1
} ql_codegen_mode;

typedef enum ql_target_feature_profile {
    QL_TARGET_FEATURE_X86_64_BASE = UINT64_C(1) << 0,
    QL_TARGET_FEATURE_DOMAIN_BASELINE = UINT64_C(1) << 1,
    QL_TARGET_FEATURE_AVX512 = UINT64_C(1) << 2,
    QL_TARGET_FEATURE_CRYPTO = UINT64_C(1) << 3,
    QL_TARGET_FEATURE_BITMANIP = UINT64_C(1) << 4,
    QL_TARGET_FEATURE_RANDOM = UINT64_C(1) << 5,
    QL_TARGET_FEATURE_TSX = UINT64_C(1) << 6,
    QL_TARGET_FEATURE_TIMING = UINT64_C(1) << 7
} ql_target_feature_profile;

#define QL_SEMANTIC_CONTRACT_SCHEMA_VERSION 1u
#define QL_PRECONDITION_SCHEMA_VERSION 1u

typedef struct ql_semantic_contract_v1 {
    size_t struct_size;
    uint32_t schema_version;
    ql_relation relation;
    ql_ub_policy ub_policy;
    uint64_t observations;
    ql_memory_observation memory_observation;
    ql_external_call_observation external_call_observation;
    ql_c_dialect_profile c_dialect;
    ql_target_abi target_abi;
    uint32_t compiler_families;
    uint32_t codegen_modes;
    uint64_t target_features;
    const char *precondition_json;
    size_t precondition_json_size;
    uint64_t reserved[4];
} ql_semantic_contract_v1;

QL_API void QL_CALL ql_semantic_contract_init(
    ql_semantic_contract_v1 *contract);
QL_API ql_status QL_CALL ql_semantic_contract_validate(
    const ql_semantic_contract_v1 *contract, ql_error *error);

typedef enum ql_verdict {
    QL_VERDICT_UNKNOWN = 0,
    QL_VERDICT_PROVED_EQUIVALENT,
    QL_VERDICT_PROVED_LEFT_REFINES_RIGHT,
    QL_VERDICT_PROVED_RIGHT_REFINES_LEFT,
    QL_VERDICT_COUNTEREXAMPLE,
    QL_VERDICT_BOUNDED_CLEAN
} ql_verdict;

typedef struct ql_outcome_v1 {
    size_t struct_size;
    uint32_t schema_version;
    ql_verdict verdict;
    uint32_t flags;
    uint64_t checked_bound;
    ql_digest evidence_digest;
    uint64_t reserved[4];
} ql_outcome_v1;

typedef uint32_t ql_ir_type_id;
typedef uint32_t ql_ir_value_id;
typedef uint32_t ql_ir_block_id;

typedef enum ql_ir_type_kind {
    QL_IR_TYPE_VOID = 0,
    QL_IR_TYPE_BOOL,
    QL_IR_TYPE_BIT_VECTOR,
    QL_IR_TYPE_FLOAT,
    QL_IR_TYPE_POINTER,
    QL_IR_TYPE_ARRAY,
    QL_IR_TYPE_TUPLE,
    QL_IR_TYPE_MEMORY,
    QL_IR_TYPE_EVENT_TRACE
} ql_ir_type_kind;

typedef enum ql_ir_effect {
    QL_IR_EFFECT_NONE = 0,
    QL_IR_EFFECT_MEMORY = UINT64_C(1) << 0,
    QL_IR_EFFECT_CALL = UINT64_C(1) << 1,
    QL_IR_EFFECT_VOLATILE = UINT64_C(1) << 2,
    QL_IR_EFFECT_ATOMIC = UINT64_C(1) << 3,
    QL_IR_EFFECT_IO = UINT64_C(1) << 4,
    QL_IR_EFFECT_UNDEFINED_BEHAVIOR = UINT64_C(1) << 5
} ql_ir_effect;

QL_API const char *QL_CALL ql_verdict_string(ql_verdict verdict);

QL_EXTERN_C_END

#endif
