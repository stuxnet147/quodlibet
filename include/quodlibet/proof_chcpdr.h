#ifndef QUODLIBET_PROOF_CHCPDR_H
#define QUODLIBET_PROOF_CHCPDR_H

#include "quodlibet/evidence.h"
#include "quodlibet/proof_method.h"
#include "quodlibet/registry.h"
#include "quodlibet/semantics.h"

QL_EXTERN_C_BEGIN

#define QL_CHCPDR_METHOD_NAME "prove.chc-pdr"
#define QL_CHCPDR_METHOD_VERSION "1"
#define QL_CHCPDR_OUTCOME_SCHEMA_VERSION 1u

#define QL_CHCPDR_DEFAULT_TIMEOUT_MS UINT64_C(10000)
#define QL_CHCPDR_DEFAULT_MAX_FRAMES UINT64_C(16)
#define QL_CHCPDR_DEFAULT_MAX_LEMMAS UINT64_C(128)
#define QL_CHCPDR_DEFAULT_MAX_QUERIES UINT64_C(512)

/* Property-directed reachability over the serialized relational loop pair.

   The transition system is the one `src/loop_proof.c` already serializes for
   its induction fast path: shared input symbols, one current-state constant
   per loop-carried PHI on each side, and entry, guard, next-state, and exit
   expressions defined over them. Where the fast path plugs one fixed
   relation per PHI into that system and asks a single induction query, this
   method synthesizes an inductive invariant: frames of lemmas are
   strengthened by blocking counterexamples-to-induction cube by cube, with
   literal-drop generalization and forward propagation, exactly the IC3/PDR
   loop. Bitwuzla answers every clause query; no second solver is
   introduced.

   The lemma vocabulary is concrete-value cubes over the paired state,
   the analyzer's own equality/offset/affine relation candidates, and
   entry-anchored sum and difference templates -- the last is what lets a
   pair like "count up" against "count down" close, whose invariant
   i + j = n names an input and is therefore outside the fast path's
   constant-coefficient vocabulary.

   The safety property is the fast path's own: reachable synchronized states
   never disagree on the continue guards, and a state in which both sides
   exit never disagrees on the exit observable. Together with the structural
   gates the serializer already enforces (one reducible pre-test loop pair,
   unconditional entry chain, direct-return exit) and the whole-IR
   non-vacuity flags, a verified fixpoint discharges the declared relation
   for every input.

   The verdict discipline:

     - a verified fixpoint is promoted to PROVED_* only under the explicit
       `trusted-backend` policy, with the recorded backend identity and
       `checked_proof: false`, exactly as the SMT product's promotions are;
       the inductive invariant is re-verified with three fresh queries
       (initiation, consecution, exclusion) before promotion, and is
       recorded in the outcome so an external checker can re-discharge it;
     - a reachable bad state is NEVER a counterexample: the trace is not
       concretized and replayed here, so the result is UNKNOWN with a
       diagnostic pointing at the refutation methods;
     - frame, lemma, query, and time limits leave UNKNOWN, never a weaker
       claim. */

typedef struct ql_chcpdr_outcome_view_v1 {
    size_t struct_size;
    uint32_t schema_version;
    ql_verdict verdict;
    ql_evidence_class evidence_class;
    /* Always zero: promotion rests on the recorded trusted backend. */
    uint32_t checked_proof;
    /* Always zero. This method never claims a counterexample. */
    uint32_t replay_confirmed;
    /* One when a fixpoint was found and its three verification queries
       answered UNSAT, whether or not the policy allowed promotion. */
    uint32_t invariant_verified;
    uint32_t frames_used;
    uint64_t lemma_count;
    uint64_t invariant_lemma_count;
    uint64_t queries_used;
    ql_digest problem_digest;
    ql_digest cache_key;
    char diagnostic[QL_ERROR_MESSAGE_CAPACITY];
    uint64_t reserved[4];
} ql_chcpdr_outcome_view_v1;

/* Method options, all optional:

     {"unsat_promotion":"none"|"trusted-backend",
      "timeout_ms":10000,
      "max_frames":16,
      "max_lemmas":128,
      "max_queries":512,
      "solver_options":"{...}"}

   `timeout_ms` bounds each solver query; the run budget bounds the whole
   method. An unknown key or a zero limit is rejected before the run. */
QL_API const ql_method_v1 *QL_CALL ql_chcpdr_method(void);
QL_API const ql_proof_method_v1 *QL_CALL ql_chcpdr_proof_method(void);
QL_API ql_status QL_CALL ql_register_chcpdr_method(ql_registry *registry,
                                                   ql_error *error);

QL_API ql_status QL_CALL ql_chcpdr_outcome_read(
    const ql_artifact *artifact, ql_chcpdr_outcome_view_v1 *view,
    ql_error *error);

QL_EXTERN_C_END

#endif
