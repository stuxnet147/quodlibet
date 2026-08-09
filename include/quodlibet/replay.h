#ifndef QUODLIBET_REPLAY_H
#define QUODLIBET_REPLAY_H

#include "quodlibet/ir.h"
#include "quodlibet/ir_interp.h"
#include "quodlibet/product.h"

QL_EXTERN_C_BEGIN

#define QL_REPLAY_SCHEMA_VERSION 1u
#define QL_REPLAY_MAX_INPUT_BYTES 32u

typedef struct ql_replay_witness ql_replay_witness;

/* One decoded input in the IR constant encoding: exact width, little-endian,
   with the last byte's unused high bits zero. A bool is one byte, 0 or 1. */
typedef struct ql_replay_value_v1 {
    size_t struct_size;
    uint32_t index;
    uint32_t left_parameter;
    uint32_t right_parameter;
    ql_source_type_kind kind;
    uint32_t bit_width;
    size_t size;
    uint8_t bytes[QL_REPLAY_MAX_INPUT_BYTES];
    uint64_t reserved[2];
} ql_replay_value_v1;

/* The observable projection of one concrete execution. `conclusive` is zero
   when the interpreter could not model the run at all, which is a statement
   about this tool and never about the function. */
typedef struct ql_replay_outcome_v1 {
    size_t struct_size;
    ql_ir_interp_outcome interp_outcome;
    ql_ir_interp_ub_reason ub_reason;
    uint32_t conclusive;
    uint32_t defined;
    uint32_t returns;
    uint32_t traps;
    uint32_t terminates;
    uint64_t trap_code;
    size_t return_value_size;
    uint8_t return_value[QL_IR_INTERP_VALUE_CAPACITY];
    uint64_t reserved[2];
} ql_replay_outcome_v1;

typedef struct ql_replay_result_v1 {
    size_t struct_size;
    uint32_t schema_version;
    /* One only when running both functions on the decoded input actually
       breaks the declared relation. A solver model that does not reproduce is
       an encoding defect, never a counterexample. */
    uint32_t violated;
    /* Zero when the replay could not decide, for example because the
       interpreter refused a construct or the decoded input does not satisfy
       the precondition. The caller must report UNKNOWN, not a verdict. */
    uint32_t conclusive;
    /* One when the typed precondition could be evaluated concretely at all,
       and separately one when it actually holds for the decoded input. A
       decoded input that does not satisfy the precondition means the solver
       and the problem disagree about the input domain. */
    uint32_t precondition_evaluated;
    uint32_t precondition_holds;
    ql_replay_outcome_v1 left;
    ql_replay_outcome_v1 right;
    uint64_t reserved[4];
} ql_replay_result_v1;

/* Decodes a quodlibet.solver-model into typed inputs. Every input the query
   declared must appear in the model; a missing or malformed assignment is a
   parse error rather than a defaulted value. */
QL_API ql_status QL_CALL ql_replay_decode_model(
    const ql_allocator *allocator, const ql_product_query *query,
    const ql_artifact *model, ql_replay_witness **output, ql_error *error);
QL_API void QL_CALL ql_replay_witness_destroy(ql_replay_witness *witness);
QL_API size_t QL_CALL ql_replay_witness_count(
    const ql_replay_witness *witness);
QL_API ql_status QL_CALL ql_replay_witness_value_at(
    const ql_replay_witness *witness, size_t index,
    ql_replay_value_v1 *output, ql_error *error);

/* Concretely executes both IR functions on the decoded input through the
   IR interpreter and evaluates the declared relation on the results. It also
   re-checks the typed precondition concretely.

   This is a separate semantic path from the SMT encoding, so a disagreement
   between the two surfaces here as `violated == 0` instead of becoming a
   wrong verdict. */
QL_API ql_status QL_CALL ql_replay_execute(
    const ql_allocator *allocator, const ql_problem *problem,
    const ql_product_query *query, const ql_ir *left_ir,
    const ql_ir *right_ir, const ql_replay_witness *witness,
    ql_replay_result_v1 *result, ql_error *error);

/* Requires result->violated. Refuses to serialize an unconfirmed model. */
QL_API ql_status QL_CALL ql_replay_counterexample_artifact_create(
    const ql_allocator *allocator, const ql_product_query *query,
    const ql_replay_witness *witness, const ql_replay_result_v1 *result,
    ql_artifact **output, ql_error *error);

QL_EXTERN_C_END

#endif
