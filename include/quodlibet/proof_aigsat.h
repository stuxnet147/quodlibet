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

QL_EXTERN_C_END

#endif
