#ifndef QUODLIBET_PROOF_EGRAPH_H
#define QUODLIBET_PROOF_EGRAPH_H

#include "quodlibet/evidence.h"
#include "quodlibet/proof_method.h"
#include "quodlibet/registry.h"
#include "quodlibet/semantics.h"

QL_EXTERN_C_BEGIN

#define QL_EGRAPH_PROOF_METHOD_NAME "prove.egraph"
#define QL_EGRAPH_PROOF_METHOD_VERSION "1"
#define QL_EGRAPH_PROOF_OUTCOME_SCHEMA_VERSION 1u

/* Equality-saturation proof over the pure term fragment.

   Both functions are lowered and converted into ONE e-graph with shared
   variables: the problem's argument correspondence maps each left parameter
   and its right counterpart onto the same variable term. The graph is
   saturated with the versioned rewrite catalogue, and the two return roots
   landing in one class is the equivalence claim.

   The claim is accepted only from the independent replay checker, never from
   the engine: `src/egraph_check.c` rebuilds its own union-find, replays the
   merge log, and re-discharges every side condition itself. The verdict is
   PROVED_* only when every recorded merge is JUSTIFIED, none is ASSUMED, and
   the checker's own replayed state puts the two roots in one class. The
   outcome then records `checked_proof: true`: the trusted computing base is
   this file's IR-to-term conversion, the replay checker, and the rule
   catalogue whose version and digest the evidence names -- the engine's
   matcher is not in it, exactly as CaDiCaL is not in the AIG path's.

   The accepted fragment is deliberately the one where root equality IS the
   selected behavior, with no residue for an axis the graph cannot see:

     - one basic block per side, ending in RETURN of a bool or bit-vector;
     - every instruction effect-free, single-result, and carried by a pure
       term operator (IDENTITY, BOOL_NOT, BV_NOT, BV_NEG, ADD, SUB, MUL,
       BV_AND, BV_OR, BV_XOR, EQ, NE, SELECT). There are no opaque leaves
       here: the normalizer may copy what it cannot model, a prover may not,
       because an unshared leaf can never merge across sides;
     - no UB_GUARD, ASSUME, memory, calls, traps, or event effects. Under
       the ASM2C_GNU_V1 profile signed +, -, and * lower with an overflow
       UB guard, so they fall outside this fragment today; unsigned and
       bitwise arithmetic, whose IR is guard-free, is the practical
       territory. Discharging guard equality alongside root equality is the
       extension that would admit signed arithmetic, and it needs comparison
       operators the term engine does not yet have;
     - a literal-true typed precondition. Equality on every input would still
       imply the relation under a narrower precondition, but an unsatisfiable
       precondition would make the claim vacuous, and this method has no
       solver to show inhabitation. All-inputs equality of total functions is
       never vacuous: the domain is the whole input space.

   Inside that fragment both sides are total, deterministic, and defined
   everywhere, so each behavior set is a singleton and return-value equality
   discharges equivalence and both refinement directions at once. Everything
   outside the fragment, and every saturation that stops on a limit or leaves
   the roots apart, is UNKNOWN -- never a counterexample, because failure to
   merge is incompleteness and not inequality. */

typedef struct ql_egraph_proof_outcome_view_v1 {
    size_t struct_size;
    uint32_t schema_version;
    ql_verdict verdict;
    ql_evidence_class evidence_class;
    /* One exactly when the replay checker justified every merge and its own
       replayed state merges the two roots. */
    uint32_t checked_proof;
    /* Always zero. This method produces no counterexamples. */
    uint32_t replay_confirmed;
    uint32_t saturation_complete;
    uint32_t rule_catalogue_version;
    uint64_t merge_count;
    uint64_t justified_count;
    uint64_t assumed_count;
    uint64_t rejected_count;
    uint64_t term_count;
    ql_digest rule_catalogue_digest;
    ql_digest problem_digest;
    ql_digest cache_key;
    char diagnostic[QL_ERROR_MESSAGE_CAPACITY];
    uint64_t reserved[4];
} ql_egraph_proof_outcome_view_v1;

/* Method options, all optional:

     {"node_limit":100000,
      "iteration_limit":16,
      "rewrite_limit":1000000}

   An unknown key or a zero limit is rejected before the run starts. */
QL_API const ql_method_v1 *QL_CALL ql_egraph_proof_method_entry(void);
QL_API const ql_proof_method_v1 *QL_CALL ql_egraph_proof_method(void);
QL_API ql_status QL_CALL ql_register_egraph_proof_method(
    ql_registry *registry, ql_error *error);

QL_API ql_status QL_CALL ql_egraph_proof_outcome_read(
    const ql_artifact *artifact, ql_egraph_proof_outcome_view_v1 *view,
    ql_error *error);

QL_EXTERN_C_END

#endif
