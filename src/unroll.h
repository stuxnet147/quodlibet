#ifndef QUODLIBET_SRC_UNROLL_H
#define QUODLIBET_SRC_UNROLL_H

/* Bounded unrolling of a cyclic typed SSA graph into an acyclic one.

   The transform clones the block graph `bound + 1` times. An edge that goes
   forward in reverse post order stays inside its copy; an edge that retreats
   advances to the next copy, and in the last copy it is routed to one shared
   cut block whose body is ASSUME(false). The product encoding conditions
   every ASSUME on its block being reached, so an input whose execution would
   traverse more than `bound` retreating edges falls out of the comparison
   domain instead of being given invented behavior. That is exactly the
   bounded-search semantics: SAT on the unrolled miter is a genuine in-bound
   violation, and UNSAT says only that no violation exists within the bound.

   Values are renamed by "latest emitted instance": copies are emitted in
   ascending order and blocks in reverse post order inside a copy, so at every
   use site the most recently emitted instance of the original SSA value is
   the defining instance on every path -- when it is not, the rebuilt graph
   fails SSA dominance validation in ql_ir_open and the caller reports the
   shape as unsupported rather than receiving a wrong program. */

#include "quodlibet/ir.h"

QL_EXTERN_C_BEGIN

/* The transform refuses to emit more block instances than this, because the
   product encoder and the solver behind it scale with the flattened graph. */
#define QL_UNROLL_MAX_BLOCKS 65536u

typedef struct ql_unroll_stats_v1 {
    size_t struct_size;
    /* Block instances emitted, including the cut block when one exists. */
    uint64_t blocks_emitted;
    /* One when at least one retreating edge was routed to the cut block, so
       the bound is actually load-bearing for this graph. Zero means the graph
       ran out of retreating edges before the bound: the unrolled program is
       exhaustive and the bound was not the limiting factor. */
    uint32_t bound_cut_used;
    /* Retreating edges in the original graph. */
    uint32_t retreating_edges;
    uint32_t reserved32;
    uint64_t reserved[4];
} ql_unroll_stats_v1;

/* Builds an acyclic IR artifact that under-approximates `ir` to executions
   traversing at most `bound` retreating edges. Parameters, constants, and
   their order are preserved, so a signature bound to the original IR binds to
   the unrolled one. An acyclic input is rebuilt unchanged (one copy, no cut).
   `stats` may be null. */
ql_status ql_ir_unroll_bounded(const ql_allocator *allocator, const ql_ir *ir,
                               uint32_t bound, ql_unroll_stats_v1 *stats,
                               ql_artifact **output, ql_error *error);

QL_EXTERN_C_END

#endif
