#ifndef QUODLIBET_PROOF_AIGSAT_H
#define QUODLIBET_PROOF_AIGSAT_H

#include "quodlibet/aig.h"
#include "quodlibet/evidence.h"
#include "quodlibet/proof_method.h"
#include "quodlibet/registry.h"
#include "quodlibet/semantics.h"

QL_EXTERN_C_BEGIN

#define QL_AIG_SAT_METHOD_NAME "prove.aig-sat"
#define QL_AIG_SAT_METHOD_VERSION "1"
#define QL_AIG_SAT_OUTCOME_SCHEMA_VERSION 1u

#define QL_AIG_SAT_DEFAULT_TIMEOUT_MS UINT64_C(60000)

/* Bit-blasting of the SMT-LIB text `ql_smt2_builder` produces.

   This deliberately does not walk the IR. `src/product.c` already encodes the
   whole contract once: block reachability, PHI, the observation axes, the UB
   policy and its totalization, the typed precondition, the relation
   direction, and the refusal of memory, effects, and non-scalar types.
   Rebuilding all of that at bit level would put the C semantics in two
   independent encoders, and a `checked_proof` that rests on the second one is
   only worth anything if the two never drift. A verified proof of the wrong
   question is worse than an unverified proof of the right one, because it
   carries authority.

   So the AIG path consumes the same query bytes the SMT path sends to
   Bitwuzla. The two backends answer the same question by construction, and the
   outcome records the same query digest for both.

   The accepted grammar is closed: exactly what `ql_smt2_builder` emits, and
   nothing else. An input outside it is refused with QL_STATUS_TYPE_MISMATCH,
   never guessed at. Array sorts are refused too, because the first cut is
   scalar. This is a bit-blaster for one producer, not a general SMT-LIB
   front end. */

typedef struct ql_aig_blast ql_aig_blast;

typedef struct ql_aig_blast_symbol_v1 {
    size_t struct_size;
    const char *name;
    size_t name_size;
    uint32_t is_bool;
    uint32_t bit_width;
    /* The AIG input index of bit zero. A declared constant's bits occupy
       consecutive input indices from here, least significant bit first. This
       is what a SAT model is decoded through. */
    uint32_t first_input;
    uint32_t reserved32;
    uint64_t reserved[2];
} ql_aig_blast_symbol_v1;

typedef struct ql_aig_blast_view_v1 {
    size_t struct_size;
    uint32_t schema_version;
    uint32_t reserved_alignment;
    /* The conjunction of every assertion in the text. */
    ql_aig_lit root;
    uint64_t declared_count;
    uint64_t defined_count;
    uint64_t assertion_count;
    uint64_t reserved[4];
} ql_aig_blast_view_v1;

/* Blasts the concatenation of `parts`, in order, into `aig`. Each part must be
   a quodlibet.smtlib2 artifact. The parts are read as one text, so a prefix
   artifact and a terminal assertion artifact blast together exactly as the
   solver would read them. */
QL_API ql_status QL_CALL ql_aig_blast_smt2(
    ql_aig *aig, const ql_artifact *const *parts, size_t part_count,
    ql_aig_blast **output, ql_error *error);
QL_API void QL_CALL ql_aig_blast_destroy(ql_aig_blast *blast);
QL_API ql_status QL_CALL ql_aig_blast_get_view(const ql_aig_blast *blast,
                                               ql_aig_blast_view_v1 *view,
                                               ql_error *error);
/* Declared constants only, in declaration order. A defined symbol is a
   circuit, not a free variable, and a model assigns it nothing. */
QL_API size_t QL_CALL ql_aig_blast_symbol_count(const ql_aig_blast *blast);
QL_API ql_status QL_CALL ql_aig_blast_symbol_at(
    const ql_aig_blast *blast, size_t index,
    ql_aig_blast_symbol_v1 *output, ql_error *error);
QL_API ql_status QL_CALL ql_aig_blast_symbol_by_name(
    const ql_aig_blast *blast, const char *name,
    ql_aig_blast_symbol_v1 *output, ql_error *error);

/* Turns a SAT assignment back into a quodlibet.solver-model artifact.

   `assignment` is the DIMACS literal list a solver prints on its `v` lines: a
   positive entry sets that variable true, a negative one sets it false, and a
   zero terminator is ignored. Order does not matter and a variable may be
   absent.

   The point of returning a solver-model artifact rather than typed values is
   that the AIG path then hands its witness to ql_replay_decode_model and
   ql_replay_execute, the same decoder and the same relation evaluator that
   validate a Bitwuzla model. A second decoder could disagree with the first
   about what a bit pattern means; there is not one.

   A declared bit the root's cone never reached has no CNF variable and no
   assignment. It is emitted as zero, which is sound precisely because the
   miter's value does not depend on it, and the replay re-derives the
   violation concretely anyway. */
QL_API ql_status QL_CALL ql_aig_blast_model_artifact_create(
    const ql_allocator *allocator, const ql_aig_blast *blast,
    const ql_aig_cnf *cnf, const int32_t *assignment, size_t assignment_count,
    ql_artifact **output, ql_error *error);

/* --- The method ----------------------------------------------------------- */

/* What a SAT query returned. `trivially-*` are answers the encoding produced
   by folding, without a solver and without a certificate. */
typedef enum ql_aig_sat_answer {
    QL_AIG_SAT_ANSWER_NOT_QUERIED = 0,
    QL_AIG_SAT_ANSWER_SAT = 1,
    QL_AIG_SAT_ANSWER_UNSAT = 2,
    QL_AIG_SAT_ANSWER_UNKNOWN = 3,
    QL_AIG_SAT_ANSWER_TRIVIALLY_TRUE = 4,
    QL_AIG_SAT_ANSWER_TRIVIALLY_FALSE = 5
} ql_aig_sat_answer;

typedef struct ql_aig_sat_outcome_view_v1 {
    size_t struct_size;
    uint32_t schema_version;
    ql_verdict verdict;
    ql_evidence_class evidence_class;
    /* One only when the LRAT certificate for this exact CNF passed the
       checker, the comparison domain was shown inhabited, and the problem
       carries a proof binding. This is the field the whole workstream is
       for; nothing else in the outcome may be read as standing in for it. */
    uint32_t checked_proof;
    uint32_t replay_confirmed;
    ql_aig_sat_answer violation_answer;
    ql_aig_sat_answer domain_answer;
    uint64_t cnf_variable_count;
    uint64_t cnf_clause_count;
    ql_digest problem_digest;
    ql_digest cache_key;
    /* The same values the SMT path records for the same problem: both
       backends are asked the same question, and the envelope says so. */
    ql_digest prefix_digest;
    ql_digest violation_digest;
    ql_digest domain_digest;
    /* The bytes that actually ran, not the paths they were read from. */
    ql_digest solver_binary_digest;
    ql_digest checker_binary_digest;
    /* The DIMACS handed to the solver and the LRAT it produced. */
    ql_digest cnf_digest;
    ql_digest proof_digest;
    ql_digest counterexample_digest;
    char diagnostic[QL_ERROR_MESSAGE_CAPACITY];
    uint64_t reserved[4];
} ql_aig_sat_outcome_view_v1;

/* Method options, all optional:

     {"timeout_ms":60000,
      "memory_limit_mb":0,
      "solver_executable":"/abs/path/to/cadical",
      "checker_executable":"/abs/path/to/lrat-check"}

   The two executable paths exist because the checker is a replaceable
   boundary: a stronger checker can be substituted without touching this
   method, and the outcome records which one ran. Both default to the pinned
   binaries this build vendored. An unknown key or a relative path is refused
   before the run starts. */
QL_API const ql_method_v1 *QL_CALL ql_aig_sat_method(void);
QL_API const ql_proof_method_v1 *QL_CALL ql_aig_sat_proof_method(void);
QL_API ql_status QL_CALL ql_aig_sat_register_method(ql_registry *registry,
                                                    ql_error *error);
/* Non-zero when this build vendored a SAT backend and a checker. Without both
   the method is registered but every run answers UNKNOWN. */
QL_API uint32_t QL_CALL ql_aig_sat_available(void);

QL_API ql_status QL_CALL ql_aig_sat_outcome_read(
    const ql_artifact *artifact, ql_aig_sat_outcome_view_v1 *view,
    ql_error *error);
QL_API ql_status QL_CALL ql_aig_sat_outcome_counterexample(
    const ql_allocator *allocator, const ql_artifact *artifact,
    ql_artifact **output, ql_error *error);

QL_EXTERN_C_END

#endif
