#ifndef QUODLIBET_TESTS_FUZZ_CONTRACT_TARGETS_H
#define QUODLIBET_TESTS_FUZZ_CONTRACT_TARGETS_H

/* Fuzz targets for the contract surfaces: the typed-precondition parser, the
   problem decoder, the source-signature decoder, and the judgement-policy
   JSON.

   These are the inputs that decide what a run is allowed to claim, so a
   malformed one that is accepted is worse than a crash. The targets therefore
   assert the soundness rules that the surfaces exist to enforce, not just the
   absence of a crash:

     - a schema v1 problem never passes ql_problem_require_proof_binding(),
       because its precondition is bound to neither source signature;
     - a policy the parser accepted never claims "proof" for a verdict the
       core did not prove, never accepts COUNTEREXAMPLE without demanding a
       replayed witness, and never accepts a PROVED_* verdict with no stated
       proof trust;
     - canonicalisation is a fixed point: re-parsing the canonical bytes of an
       accepted precondition yields the same digest, so the same contract
       cannot acquire two identities.

   The layout mirrors tests/fuzz/fuzz_targets.h: the bodies live here so that
   the libFuzzer drivers in this directory and the deterministic campaign in
   tests/test_fuzz_contracts.cpp run exactly the same code, and both includers
   override the two hooks below. This is a separate header from
   fuzz_targets.h because that one records the parser, lowering and IR
   surfaces, which a different workstream owns. */

#include "quodlibet/artifact.h"
#include "quodlibet/policy.h"
#include "quodlibet/precondition.h"
#include "quodlibet/problem.h"
#include "quodlibet/signature.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if !defined(QL_FUZZ_REQUIRE)
#define QL_FUZZ_REQUIRE(condition_, message_)                                \
    do {                                                                     \
        if (!(condition_)) {                                                 \
            fprintf(stderr, "quodlibet fuzz invariant violated: %s\n",        \
                    (message_));                                             \
            abort();                                                         \
        }                                                                    \
    } while (0)
#endif

/* `kind_` is one of precondition, problem_v1, problem_v2, signature,
   policy, policy_result. */
#if !defined(QL_FUZZ_REACHED)
#define QL_FUZZ_REACHED(kind_) ((void)0)
#endif

#if defined(__GNUC__) || defined(__clang__)
#define QL_FUZZ_MAYBE_UNUSED __attribute__((unused))
#else
#define QL_FUZZ_MAYBE_UNUSED
#endif

/* Walks every node of an accepted precondition and checks the structural
   promises the view makes. A parser that accepted an input but left a child
   index dangling would hand a later consumer an out-of-range read. */
QL_FUZZ_MAYBE_UNUSED static void ql_fuzz_precondition_walk(
    const ql_precondition *precondition,
    const ql_precondition_view_v1 *view) {
    ql_error error;
    size_t index;

    QL_FUZZ_REQUIRE(view->node_count > 0u,
                    "an accepted precondition has no nodes");
    QL_FUZZ_REQUIRE(view->root_node < view->node_count,
                    "an accepted precondition's root is out of range");
    QL_FUZZ_REQUIRE(view->canonical_bytes != NULL && view->canonical_size > 0u,
                    "an accepted precondition has no canonical bytes");
    for (index = 0u; index < view->node_count; ++index) {
        ql_precondition_node_view_v1 node;
        size_t child;

        memset(&node, 0, sizeof(node));
        node.struct_size = sizeof(node);
        QL_FUZZ_REQUIRE(ql_precondition_node_at(precondition, index, &node,
                                                &error) == QL_STATUS_OK,
                        "a node inside an accepted precondition's node count "
                        "could not be read");
        for (child = 0u; child < node.child_count; ++child) {
            QL_FUZZ_REQUIRE(node.children != NULL,
                            "a precondition node reports children but has no "
                            "child array");
            QL_FUZZ_REQUIRE(node.children[child] < view->node_count,
                            "a precondition node points at a node index that "
                            "does not exist");
        }
    }
}

/* Derives a signature from the leading bytes so the type checker is fuzzed
   alongside the JSON. Parsing against one fixed signature would leave every
   type error on a path the campaign never varies. Returns the number of bytes
   consumed. */
QL_FUZZ_MAYBE_UNUSED static size_t ql_fuzz_precondition_signature(
    const unsigned char *data, size_t size, ql_signature_view_v1 *view,
    ql_signature_argument_v1 *storage, size_t capacity) {
    static const ql_signature_argument_kind kinds[] = {
        QL_SIGNATURE_ARGUMENT_BOOL, QL_SIGNATURE_ARGUMENT_SIGNED_INTEGER,
        QL_SIGNATURE_ARGUMENT_UNSIGNED_INTEGER, QL_SIGNATURE_ARGUMENT_POINTER};
    static const uint32_t widths[] = {8u, 16u, 32u, 64u};
    size_t count;
    size_t index;
    uint32_t pointer_width;

    ql_signature_view_init(view);
    if (size == 0u) {
        view->arguments = NULL;
        view->argument_count = 0u;
        return 0u;
    }
    pointer_width = (data[0] & 1u) != 0u ? 64u : 32u;
    view->pointer_width = pointer_width;
    count = (size_t)(data[0] >> 1) % (capacity + 1u);
    if (count + 1u > size) {
        count = size - 1u;
    }
    for (index = 0u; index < count; ++index) {
        const unsigned char byte = data[index + 1u];
        ql_signature_argument_init(&storage[index]);
        storage[index].kind = kinds[byte & 3u];
        if (storage[index].kind == QL_SIGNATURE_ARGUMENT_POINTER) {
            storage[index].bit_width = pointer_width;
            storage[index].address_space = (uint32_t)((byte >> 2) & 3u);
        } else if (storage[index].kind == QL_SIGNATURE_ARGUMENT_BOOL) {
            storage[index].bit_width = 1u;
        } else {
            storage[index].bit_width =
                widths[(byte >> 2) & 3u];
        }
    }
    view->arguments = count == 0u ? NULL : storage;
    view->argument_count = count;
    return count + 1u;
}

#define QL_FUZZ_MAX_SIGNATURE_ARGUMENTS 16u

QL_FUZZ_MAYBE_UNUSED static void ql_fuzz_precondition(
    const unsigned char *data, size_t size) {
    ql_signature_argument_v1 storage[QL_FUZZ_MAX_SIGNATURE_ARGUMENTS];
    ql_signature_view_v1 signature;
    ql_precondition *precondition = NULL;
    ql_precondition *again = NULL;
    ql_precondition_view_v1 view;
    ql_precondition_view_v1 second;
    ql_error error;
    size_t consumed;

    memset(storage, 0, sizeof(storage));
    consumed = ql_fuzz_precondition_signature(
        data, size, &signature, storage, QL_FUZZ_MAX_SIGNATURE_ARGUMENTS);
    if (ql_signature_view_validate(&signature, &error) != QL_STATUS_OK) {
        return;
    }
    if (ql_precondition_parse(NULL, (const char *)data + consumed,
                              size - consumed, &signature, &precondition,
                              &error) != QL_STATUS_OK) {
        ql_precondition_destroy(precondition);
        return;
    }
    QL_FUZZ_REACHED(precondition);
    memset(&view, 0, sizeof(view));
    view.struct_size = sizeof(view);
    QL_FUZZ_REQUIRE(
        ql_precondition_get_view(precondition, &view, &error) == QL_STATUS_OK,
        "an accepted precondition refused to produce a view");
    ql_fuzz_precondition_walk(precondition, &view);

    /* Canonicalisation must be a fixed point. If the canonical form of an
       accepted input parsed to a different digest, the same contract would
       have two identities and a cache keyed on the digest would answer for
       the wrong one. */
    if (ql_precondition_parse(NULL, view.canonical_bytes, view.canonical_size,
                              &signature, &again, &error) == QL_STATUS_OK) {
        memset(&second, 0, sizeof(second));
        second.struct_size = sizeof(second);
        if (ql_precondition_get_view(again, &second, &error) ==
            QL_STATUS_OK) {
            QL_FUZZ_REQUIRE(second.canonical_size == view.canonical_size &&
                                memcmp(second.canonical_bytes,
                                       view.canonical_bytes,
                                       view.canonical_size) == 0,
                            "re-parsing a precondition's canonical bytes "
                            "produced different canonical bytes");
            QL_FUZZ_REQUIRE(memcmp(&second.digest, &view.digest,
                                   sizeof(view.digest)) == 0,
                            "re-parsing a precondition's canonical bytes "
                            "produced a different digest");
        }
    } else {
        QL_FUZZ_REQUIRE(0,
                        "the canonical bytes of an accepted precondition were "
                        "rejected by the parser that produced them");
    }
    ql_precondition_destroy(again);
    ql_precondition_destroy(precondition);
}

/* One decode attempt at one declared artifact schema version. */
QL_FUZZ_MAYBE_UNUSED static void ql_fuzz_problem_decode_at(
    const unsigned char *data, size_t size, uint32_t schema) {
    ql_artifact *artifact = NULL;
    ql_problem *problem = NULL;
    ql_problem_view_v1 view;
    ql_problem_view_v2 view2;
    ql_error error;
    ql_status status;

    if (ql_artifact_create(NULL, QL_ARTIFACT_KIND_PROBLEM, schema, data, size,
                           &artifact, &error) != QL_STATUS_OK) {
        return;
    }
    if (ql_problem_open(NULL, artifact, &problem, &error) != QL_STATUS_OK) {
        ql_problem_release(problem);
        ql_artifact_release(artifact);
        return;
    }
    memset(&view, 0, sizeof(view));
    view.struct_size = sizeof(view);
    QL_FUZZ_REQUIRE(
        ql_problem_get_view(problem, &view, &error) == QL_STATUS_OK,
        "an opened problem refused to produce a schema v1 view");

    memset(&view2, 0, sizeof(view2));
    view2.struct_size = sizeof(view2);
    status = ql_problem_get_view_v2(problem, &view2, &error);
    if (view.schema_version == QL_PROBLEM_SCHEMA_VERSION) {
        QL_FUZZ_REACHED(problem_v1);
        QL_FUZZ_REQUIRE(status == QL_STATUS_SCHEMA_MISMATCH,
                        "a schema v1 problem produced a schema v2 view");
        /* The gate every proof method passes before emitting PROVED_*. A v1
           problem binds its precondition to neither source signature and
           records no argument correspondence, so no decoded byte string may
           make it pass. */
        QL_FUZZ_REQUIRE(
            ql_problem_require_proof_binding(problem, &error) != QL_STATUS_OK,
            "a decoded schema v1 problem passed the proof-binding gate");
        QL_FUZZ_REQUIRE(ql_problem_left_signature_artifact(problem) == NULL &&
                            ql_problem_right_signature_artifact(problem) ==
                                NULL,
                        "a schema v1 problem carried a source signature");
    } else if (status == QL_STATUS_OK) {
        size_t index;
        QL_FUZZ_REACHED(problem_v2);
        QL_FUZZ_REQUIRE(view2.schema_version == QL_PROBLEM_SCHEMA_VERSION_2,
                        "a schema v2 view reported another schema version");
        QL_FUZZ_REQUIRE(
            ql_problem_left_signature_artifact(problem) != NULL &&
                ql_problem_right_signature_artifact(problem) != NULL,
            "a schema v2 problem is missing a rebuilt source signature");
        for (index = 0u; index < view2.argument_binding_count; ++index) {
            ql_problem_argument_binding_v1 binding;
            memset(&binding, 0, sizeof(binding));
            binding.struct_size = sizeof(binding);
            QL_FUZZ_REQUIRE(ql_problem_argument_binding_at(
                                problem, index, &binding, &error) ==
                                QL_STATUS_OK,
                            "an argument binding inside the reported count "
                            "could not be read");
        }
    }
    ql_problem_release(problem);
    ql_artifact_release(artifact);
}

/* The artifact header carries the schema version and the payload states it
   again, so a mutated payload only reaches the decoder when the two agree.
   Trying both is what keeps each schema's rules reachable rather than leaving
   whichever one the seed's first bytes happen to miss untested. */
QL_FUZZ_MAYBE_UNUSED static void ql_fuzz_problem_decoder(
    const unsigned char *data, size_t size) {
    ql_fuzz_problem_decode_at(data, size, QL_PROBLEM_SCHEMA_VERSION);
    ql_fuzz_problem_decode_at(data, size, QL_PROBLEM_SCHEMA_VERSION_2);
}

QL_FUZZ_MAYBE_UNUSED static void ql_fuzz_signature_decoder(
    const unsigned char *data, size_t size) {
    ql_artifact *artifact = NULL;
    ql_source_signature *signature = NULL;
    ql_source_signature_view_v1 view;
    ql_signature_argument_v1 storage[QL_SOURCE_SIGNATURE_MAX_ARGUMENTS];
    ql_signature_view_v1 precondition_signature;
    ql_error error;
    size_t index;

    if (ql_artifact_create(NULL, QL_ARTIFACT_KIND_SOURCE_SIGNATURE,
                           QL_SOURCE_SIGNATURE_SCHEMA_VERSION, data, size,
                           &artifact, &error) != QL_STATUS_OK) {
        return;
    }
    if (ql_source_signature_open(NULL, artifact, &signature, &error) !=
        QL_STATUS_OK) {
        ql_source_signature_release(signature);
        ql_artifact_release(artifact);
        return;
    }
    QL_FUZZ_REACHED(signature);
    memset(&view, 0, sizeof(view));
    view.struct_size = sizeof(view);
    QL_FUZZ_REQUIRE(
        ql_source_signature_get_view(signature, &view, &error) ==
            QL_STATUS_OK,
        "an opened source signature refused to produce a view");
    QL_FUZZ_REQUIRE(view.argument_count <= QL_SOURCE_SIGNATURE_MAX_ARGUMENTS,
                    "an opened source signature reports more arguments than "
                    "the schema permits");
    QL_FUZZ_REQUIRE(view.c_dialect != QL_C_DIALECT_INVALID &&
                        view.target_abi != QL_TARGET_ABI_INVALID,
                    "an opened source signature carries an invalid ABI "
                    "profile");
    for (index = 0u; index < view.argument_count; ++index) {
        ql_source_type_v1 argument;
        memset(&argument, 0, sizeof(argument));
        argument.struct_size = sizeof(argument);
        QL_FUZZ_REQUIRE(ql_source_signature_argument_at(signature, index,
                                                        &argument, &error) ==
                            QL_STATUS_OK,
                        "an argument inside the reported count could not be "
                        "read");
        if (argument.kind == QL_SOURCE_TYPE_POINTER) {
            QL_FUZZ_REQUIRE(argument.bit_width == view.pointer_width,
                            "a decoded pointer argument does not use the "
                            "signature's pointer width");
        }
    }
    /* The bridge the typed precondition is checked against. It must never
       hand back a view describing more arguments than it filled. */
    memset(storage, 0, sizeof(storage));
    memset(&precondition_signature, 0, sizeof(precondition_signature));
    if (ql_source_signature_precondition_view(
            signature, &precondition_signature, storage,
            QL_SOURCE_SIGNATURE_MAX_ARGUMENTS, &error) == QL_STATUS_OK) {
        QL_FUZZ_REQUIRE(precondition_signature.argument_count <=
                            view.argument_count,
                        "the precondition view describes more arguments than "
                        "the signature has");
        QL_FUZZ_REQUIRE(ql_signature_view_validate(&precondition_signature,
                                                   &error) == QL_STATUS_OK,
                        "the precondition view derived from a decoded "
                        "signature does not validate");
    }
    ql_source_signature_release(signature);
    ql_artifact_release(artifact);
}

/* The three promotions schema v1 forbids, restated over the parsed policy.
   The parser documents them as rejections; this checks that no byte string
   reaches an accepted policy that still contains one. */
QL_FUZZ_MAYBE_UNUSED static void ql_fuzz_policy_class(
    const ql_policy_class_view_v1 *class_view) {
    uint32_t index;
    int accepts_counterexample = 0;
    int accepts_proved = 0;

    for (index = 0u; index < class_view->verdict_count; ++index) {
        const ql_verdict verdict = class_view->verdicts[index];
        const int proved = verdict == QL_VERDICT_PROVED_EQUIVALENT ||
                           verdict == QL_VERDICT_PROVED_LEFT_REFINES_RIGHT ||
                           verdict == QL_VERDICT_PROVED_RIGHT_REFINES_LEFT;
        if (class_view->claims == QL_POLICY_CLAIMS_PROOF) {
            QL_FUZZ_REQUIRE(proved,
                            "an accepted policy claims proof for a verdict "
                            "the core did not prove");
        }
        if (verdict == QL_VERDICT_COUNTEREXAMPLE) {
            accepts_counterexample = 1;
        }
        if (proved) {
            accepts_proved = 1;
        }
    }
    if (accepts_counterexample) {
        QL_FUZZ_REQUIRE(class_view->require_replayed_witness != 0u,
                        "an accepted policy takes a counterexample without "
                        "requiring a replayed witness");
    }
    /* Only a class that banks on the proved verdict has to say why it trusts
       it: one that counts it as a pass, or one that claims proof strength. A
       class that abstains on proved_* promotes nothing, which is why the
       parser scopes the rule this way rather than to every mention. */
    if (accepts_proved &&
        (class_view->disposition == QL_POLICY_DISPOSITION_PASS ||
         class_view->claims == QL_POLICY_CLAIMS_PROOF)) {
        QL_FUZZ_REQUIRE(class_view->proof_trust !=
                            QL_POLICY_PROOF_TRUST_UNSET,
                        "an accepted policy passes a proved verdict with no "
                        "stated proof trust");
    }
    if (!accepts_proved) {
        QL_FUZZ_REQUIRE(class_view->proof_trust ==
                            QL_POLICY_PROOF_TRUST_UNSET,
                        "an accepted policy states proof trust on a class "
                        "that accepts no proved verdict");
    }
}

QL_FUZZ_MAYBE_UNUSED static void ql_fuzz_policy(const unsigned char *data,
                                                 size_t size) {
    ql_policy *policy = NULL;
    ql_error error;
    size_t count;
    size_t index;
    size_t written = 0u;
    char buffer[8192];

    if (ql_policy_parse(NULL, (const char *)data, size, &policy, &error) !=
        QL_STATUS_OK) {
        ql_policy_destroy(policy);
        return;
    }
    QL_FUZZ_REACHED(policy);
    count = ql_policy_class_count(policy);
    QL_FUZZ_REQUIRE(count > 0u && count <= QL_POLICY_MAX_CLASSES,
                    "an accepted policy has an out-of-range class count");
    for (index = 0u; index < count; ++index) {
        ql_policy_class_view_v1 class_view;
        memset(&class_view, 0, sizeof(class_view));
        class_view.struct_size = sizeof(class_view);
        QL_FUZZ_REQUIRE(ql_policy_class_at(policy, index, &class_view,
                                           &error) == QL_STATUS_OK,
                        "a class inside the reported count could not be read");
        ql_fuzz_policy_class(&class_view);
    }
    /* The canonical form must parse back and reach the same bytes on the
       first round trip, not eventually. A policy whose text kept changing
       every time it was read and written would not be the same policy the
       next run loads, and a store keyed on that text would answer for the
       wrong one.

       This was once weakened to check the fixed point from the second
       serialisation onward, because a score of -0.0 was written as `-0`,
       read back as the integer 0 and rewritten as `0`. src/policy.c now
       collapses both zeros in the canonical writer, so the strong form holds
       and is what is checked.

       `written` counts the terminator, which is not part of the JSON. */
    if (ql_policy_serialize(policy, buffer, sizeof(buffer), &written,
                            &error) == QL_STATUS_OK) {
        ql_policy *again = NULL;
        const size_t length = written - 1u;
        if (ql_policy_parse(NULL, buffer, length, &again, &error) ==
            QL_STATUS_OK) {
            char second[8192];
            size_t second_written = 0u;
            if (ql_policy_serialize(again, second, sizeof(second),
                                    &second_written, &error) ==
                QL_STATUS_OK) {
                QL_FUZZ_REQUIRE(second_written == written &&
                                    memcmp(second, buffer, length) == 0,
                                "an accepted policy's canonical form is not a "
                                "fixed point of one round trip");
            }
        } else {
            QL_FUZZ_REQUIRE(0,
                            "the serialisation of an accepted policy was "
                            "rejected by the parser that produced it");
        }
        ql_policy_destroy(again);
    }
    ql_policy_destroy(policy);
}

QL_FUZZ_MAYBE_UNUSED static void ql_fuzz_policy_result(
    const unsigned char *data, size_t size) {
    ql_policy_result_v1 result;
    ql_error error;
    char buffer[4096];
    size_t written = 0u;

    ql_policy_result_init(&result);
    if (ql_policy_result_parse((const char *)data, size, &result, &error) !=
        QL_STATUS_OK) {
        return;
    }
    QL_FUZZ_REACHED(policy_result);
    /* A parsed result must never say a verdict was proved while also saying
       the run was gated back to UNKNOWN: the two together would let a caller
       read a proof out of a withdrawn run. */
    if (result.gated != 0u) {
        QL_FUZZ_REQUIRE(result.effective_verdict == QL_VERDICT_UNKNOWN,
                        "a gated policy result kept a verdict other than "
                        "unknown");
    }
    if (ql_policy_result_serialize(&result, buffer, sizeof(buffer), &written,
                                   &error) == QL_STATUS_OK) {
        ql_policy_result_v1 again;
        ql_policy_result_init(&again);
        QL_FUZZ_REQUIRE(ql_policy_result_parse(buffer, written - 1u, &again,
                                               &error) == QL_STATUS_OK,
                        "the serialisation of an accepted policy result was "
                        "rejected by the parser that produced it");
        QL_FUZZ_REQUIRE(again.effective_verdict == result.effective_verdict &&
                            again.reported_verdict ==
                                result.reported_verdict &&
                            again.disposition == result.disposition &&
                            again.claims == result.claims,
                        "a policy result changed meaning across a "
                        "serialisation round trip");
    }
}

#endif
