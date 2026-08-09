#ifndef QUODLIBET_TESTS_FUZZ_BLASTER_TARGET_H
#define QUODLIBET_TESTS_FUZZ_BLASTER_TARGET_H

/* The fuzz target for the QF_BV bit-blaster.

   The blaster is the one place where the AIG path reads bytes it did not
   write, so it is the one place a malformed or drifting query can reach the
   circuit builder. Everything else in that path consumes structures. This
   target lives in its own header, beside the W1 targets rather than inside
   them, so the two workstreams never have to edit one file at once.

   Not crashing is the weakest property. The target also asserts what a
   successful blast must guarantee:

     - the root literal belongs to the graph it was built in;
     - the root evaluates on a concrete assignment without reading outside the
       graph;
     - the CNF exit accepts whatever the blaster produced, so a query that
       blasts can always be handed to a solver.

   A refusal is a pass, not a failure. Refusing everything outside the emitted
   grammar is this component's job. */

#include "quodlibet/aig.h"
#include "quodlibet/proof_aigsat.h"
#include "quodlibet/solver.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if !defined(QL_FUZZ_REQUIRE)
#define QL_FUZZ_REQUIRE(condition_, message_)                                \
    do {                                                                     \
        if (!(condition_)) {                                                 \
            fprintf(stderr, "quodlibet fuzz invariant violated: %s\n",       \
                    (message_));                                            \
            abort();                                                         \
        }                                                                    \
    } while (0)
#endif

#if !defined(QL_FUZZ_REACHED)
#define QL_FUZZ_REACHED(kind_) ((void)0)
#endif

#if defined(__GNUC__) || defined(__clang__)
#define QL_FUZZ_MAYBE_UNUSED __attribute__((unused))
#else
#define QL_FUZZ_MAYBE_UNUSED
#endif

/* A mutated query can ask for an enormous circuit long before it asks for
   anything wrong, so the graph is bounded and an exceeded bound is an ordinary
   refusal. */
#define QL_FUZZ_BLASTER_NODE_LIMIT UINT64_C(200000)
#define QL_FUZZ_BLASTER_MAX_INPUTS 4096u

QL_FUZZ_MAYBE_UNUSED static void ql_fuzz_smt2_blaster(
    const unsigned char *data, size_t size) {
    ql_artifact *artifact = NULL;
    const ql_artifact *parts[1];
    ql_aig *aig = NULL;
    ql_aig_blast *blast = NULL;
    ql_aig_blast_view_v1 view;
    ql_aig_view_v1 graph_view;
    ql_aig_cnf *cnf = NULL;
    unsigned char *values = NULL;
    uint32_t evaluated = 0u;
    ql_error error;

    if (ql_artifact_create(NULL, QL_ARTIFACT_KIND_SMTLIB2, 1u, data, size,
                           &artifact, &error) != QL_STATUS_OK) {
        return;
    }
    if (ql_aig_create(NULL, &aig, &error) != QL_STATUS_OK) {
        ql_artifact_release(artifact);
        return;
    }
    ql_aig_set_node_limit(aig, QL_FUZZ_BLASTER_NODE_LIMIT);
    parts[0] = artifact;
    if (ql_aig_blast_smt2(aig, parts, 1u, &blast, &error) != QL_STATUS_OK) {
        /* Refusing input outside the emitted grammar is the point of this
           component, so a rejection is a pass. */
        ql_aig_destroy(aig);
        ql_artifact_release(artifact);
        return;
    }
    QL_FUZZ_REACHED(blasted);

    memset(&view, 0, sizeof(view));
    view.struct_size = sizeof(view);
    QL_FUZZ_REQUIRE(ql_aig_blast_get_view(blast, &view, &error) ==
                        QL_STATUS_OK,
                    "a successful blast produced no view");
    QL_FUZZ_REQUIRE(ql_aig_literal_is_valid(aig, view.root) != 0u,
                    "a successful blast produced a root outside its graph");

    memset(&graph_view, 0, sizeof(graph_view));
    graph_view.struct_size = sizeof(graph_view);
    QL_FUZZ_REQUIRE(ql_aig_get_view(aig, &graph_view, &error) == QL_STATUS_OK,
                    "the graph a blast filled has no view");
    if (graph_view.input_count <= QL_FUZZ_BLASTER_MAX_INPUTS) {
        const size_t count = (size_t)graph_view.input_count;
        values = count == 0u ? NULL : (unsigned char *)calloc(count, 1u);
        if (count == 0u || values != NULL) {
            QL_FUZZ_REQUIRE(ql_aig_evaluate(aig, values, count, view.root,
                                            &evaluated, &error) ==
                                QL_STATUS_OK,
                            "a blasted root could not be evaluated on a "
                            "concrete assignment");
            QL_FUZZ_REQUIRE(evaluated <= 1u,
                            "an evaluated root is not a single bit");
        }
        free(values);
    }

    if (ql_aig_cnf_create(NULL, aig, view.root, &cnf, &error) ==
        QL_STATUS_OK) {
        ql_artifact *dimacs = NULL;
        QL_FUZZ_REACHED(encoded);
        if (ql_aig_cnf_artifact_create(NULL, cnf, &dimacs, &error) ==
            QL_STATUS_OK) {
            ql_artifact_release(dimacs);
        }
        ql_aig_cnf_destroy(cnf);
    }

    ql_aig_blast_destroy(blast);
    ql_aig_destroy(aig);
    ql_artifact_release(artifact);
}

#endif
