#ifndef QUODLIBET_PROOF_BOUNDED_H
#define QUODLIBET_PROOF_BOUNDED_H

#include "quodlibet/evidence.h"
#include "quodlibet/proof_method.h"
#include "quodlibet/registry.h"
#include "quodlibet/semantics.h"

QL_EXTERN_C_BEGIN

#define QL_BOUNDED_METHOD_NAME "search.bounded-symbolic"
#define QL_BOUNDED_METHOD_VERSION "1"
#define QL_BOUNDED_OUTCOME_SCHEMA_VERSION 1u

#define QL_BOUNDED_DEFAULT_UNROLL_BOUND UINT64_C(16)
#define QL_BOUNDED_MAX_UNROLL_BOUND UINT64_C(1024)
#define QL_BOUNDED_DEFAULT_TIMEOUT_MS UINT64_C(30000)

/* Bounded symbolic search over cyclic programs.

   Each side's lowered IR is unrolled into an acyclic graph covering every
   execution that traverses at most `unroll_bound` retreating edges; the
   traversal after the bound reaches ASSUME(false), so inputs beyond the bound
   leave the comparison domain instead of being given invented behavior. The
   unrolled pair is encoded by the same relational product encoder the SMT
   method uses and the violation query is put to the pinned Bitwuzla backend.

   The verdict discipline is exactly the one METHODS.md assigns to a bounded
   search:

     - SAT is a candidate. The model is decoded and replayed concretely
       against the ORIGINAL cyclic IR, and only a replay that reproduces the
       violation is QL_VERDICT_COUNTEREXAMPLE. The counterexample is therefore
       a statement about the real functions, not about the unrolling.
     - UNSAT is QL_VERDICT_BOUNDED_CLEAN with the bound recorded. It is never
       promoted to a proof, by this method or by any policy: the unrolled
       program is an under-approximation and the UNSAT says nothing about
       executions past the bound. When neither side's graph reached the bound
       the outcome records `bound_cut_used = 0`, and the verdict still stays
       BOUNDED_CLEAN: promotion is a combiner and policy decision this method
       does not take.
     - Everything else is QL_VERDICT_UNKNOWN with a diagnostic.

   Loop-free inputs are accepted and rebuilt unchanged, so the method is total
   over the product encoder's fragment; its value over `prove.smt-product` is
   the cyclic territory the loop-free encoder refuses. */

typedef struct ql_bounded_outcome_view_v1 {
    size_t struct_size;
    uint32_t schema_version;
    ql_verdict verdict;
    ql_evidence_class evidence_class;
    /* Always zero: an UNSAT within a bound is not a proof of anything. */
    uint32_t checked_proof;
    uint32_t replay_confirmed;
    /* The retreating-edge traversal bound both sides were unrolled to. */
    uint64_t unroll_bound;
    /* One when at least one side actually routed an edge to its cut block,
       so the bound constrained the search. Zero means both graphs were
       exhausted before the bound. */
    uint32_t bound_cut_used;
    uint32_t reserved32;
    /* Block instances emitted per side, cut blocks included. */
    uint64_t left_blocks_emitted;
    uint64_t right_blocks_emitted;
    ql_digest problem_digest;
    ql_digest cache_key;
    ql_digest counterexample_digest;
    char diagnostic[QL_ERROR_MESSAGE_CAPACITY];
    uint64_t reserved[4];
} ql_bounded_outcome_view_v1;

/* Method options, all optional:

     {"unroll_bound":16,
      "timeout_ms":30000,
      "memory_limit_mb":0,
      "solver_options":"{...}"}

   An unknown key is rejected before the run starts, as is an unroll bound of
   zero or one above QL_BOUNDED_MAX_UNROLL_BOUND. */
QL_API const ql_method_v1 *QL_CALL ql_bounded_method(void);
QL_API const ql_proof_method_v1 *QL_CALL ql_bounded_proof_method(void);
QL_API ql_status QL_CALL ql_register_bounded_method(ql_registry *registry,
                                                    ql_error *error);

QL_API ql_status QL_CALL ql_bounded_outcome_read(
    const ql_artifact *artifact, ql_bounded_outcome_view_v1 *view,
    ql_error *error);
/* The embedded counterexample as its own artifact, or null when the outcome
   carries none. */
QL_API ql_status QL_CALL ql_bounded_outcome_counterexample(
    const ql_allocator *allocator, const ql_artifact *artifact,
    ql_artifact **output, ql_error *error);

QL_EXTERN_C_END

#endif
