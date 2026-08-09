#include "quodlibet/proof_diff.h"

#include <stdio.h>
#include <string.h>

#include "quodlibet/c_lower.h"
#include "quodlibet/log.h"
#include "quodlibet/problem.h"
#include "quodlibet/product.h"
#include "quodlibet/replay.h"
#include "quodlibet/solver.h"

#include "yyjson.h"

#define QL_DIFF_CATEGORY "proof.concrete-differential"
/* Boundary patterns tried before the generator starts drawing random bits.
   Equivalence bugs cluster at zero, at the sign boundary, and at the width
   limits far more densely than a uniform draw would find them. */
#define QL_DIFF_PATTERN_COUNT 12u

typedef struct diff_instance {
    ql_allocator allocator;
    uint64_t seed;
    uint64_t tests;
} diff_instance;

typedef struct diff_decision {
    ql_verdict verdict;
    ql_evidence_class evidence_class;
    uint32_t replay_confirmed;
    uint64_t tests_executed;
    uint64_t tests_conclusive;
    uint64_t tests_precondition_rejected;
    char diagnostic[QL_ERROR_MESSAGE_CAPACITY];
} diff_decision;

/* --- yyjson glue ---------------------------------------------------------- */

static void *diff_json_allocate(void *context, size_t size) {
    ql_allocator *allocator = (ql_allocator *)context;
    return allocator->allocate(allocator->user_data, size);
}

static void *diff_json_reallocate(void *context, void *pointer,
                                  size_t old_size, size_t size) {
    ql_allocator *allocator = (ql_allocator *)context;
    (void)old_size;
    return allocator->reallocate(allocator->user_data, pointer, size);
}

static void diff_json_deallocate(void *context, void *pointer) {
    ql_allocator *allocator = (ql_allocator *)context;
    allocator->deallocate(allocator->user_data, pointer);
}

static yyjson_alc diff_json_allocator(ql_allocator *allocator) {
    yyjson_alc result;
    result.malloc = diff_json_allocate;
    result.realloc = diff_json_reallocate;
    result.free = diff_json_deallocate;
    result.ctx = allocator;
    return result;
}

static void set_diagnostic(diff_decision *decision, const char *message) {
    size_t length = strlen(message);
    if (length >= sizeof(decision->diagnostic)) {
        length = sizeof(decision->diagnostic) - 1u;
    }
    memcpy(decision->diagnostic, message, length);
    decision->diagnostic[length] = '\0';
}

/* --- Options -------------------------------------------------------------- */

static ql_status parse_options(diff_instance *instance,
                               const char *options_json, ql_error *error) {
    static const char *const known[] = {"seed", "tests"};
    yyjson_alc json_allocator = diff_json_allocator(&instance->allocator);
    yyjson_doc *document;
    yyjson_read_err read_error;
    yyjson_val *root;
    yyjson_val *value;
    yyjson_obj_iter iterator;
    yyjson_val *key;
    ql_status status = QL_STATUS_OK;

    if (options_json == NULL || options_json[0] == '\0') {
        return QL_STATUS_OK;
    }
    document = yyjson_read_opts((char *)(uintptr_t)options_json,
                                strlen(options_json), 0u, &json_allocator,
                                &read_error);
    if (document == NULL) {
        ql_error_set(error, QL_STATUS_PARSE_ERROR,
                     "invalid %s options at byte %zu: %s", QL_DIFF_METHOD_NAME,
                     read_error.pos,
                     read_error.msg != NULL ? read_error.msg : "parse error");
        return QL_STATUS_PARSE_ERROR;
    }
    root = yyjson_doc_get_root(document);
    if (!yyjson_is_obj(root)) {
        ql_error_set(error, QL_STATUS_PARSE_ERROR,
                     "%s options must be a JSON object", QL_DIFF_METHOD_NAME);
        status = QL_STATUS_PARSE_ERROR;
        goto cleanup;
    }
    yyjson_obj_iter_init(root, &iterator);
    while ((key = yyjson_obj_iter_next(&iterator)) != NULL) {
        const char *name = yyjson_get_str(key);
        size_t index;
        int recognized = 0;
        for (index = 0u; index < sizeof(known) / sizeof(known[0]); ++index) {
            if (name != NULL && strcmp(name, known[index]) == 0) {
                recognized = 1;
                break;
            }
        }
        if (!recognized) {
            ql_error_set(error, QL_STATUS_INVALID_ARGUMENT,
                         "%s does not accept the option '%s'",
                         QL_DIFF_METHOD_NAME, name != NULL ? name : "");
            status = QL_STATUS_INVALID_ARGUMENT;
            goto cleanup;
        }
    }

    value = yyjson_obj_get(root, "seed");
    if (value != NULL) {
        if (!yyjson_is_uint(value)) {
            ql_error_set(error, QL_STATUS_INVALID_ARGUMENT,
                         "seed must be a non-negative integer");
            status = QL_STATUS_INVALID_ARGUMENT;
            goto cleanup;
        }
        instance->seed = yyjson_get_uint(value);
    }
    value = yyjson_obj_get(root, "tests");
    if (value != NULL) {
        if (!yyjson_is_uint(value)) {
            ql_error_set(error, QL_STATUS_INVALID_ARGUMENT,
                         "tests must be a non-negative integer");
            status = QL_STATUS_INVALID_ARGUMENT;
            goto cleanup;
        }
        instance->tests = yyjson_get_uint(value);
        if (instance->tests > QL_DIFF_MAX_TESTS) {
            ql_error_set(error, QL_STATUS_INVALID_ARGUMENT,
                         "tests must not exceed %llu",
                         (unsigned long long)QL_DIFF_MAX_TESTS);
            status = QL_STATUS_INVALID_ARGUMENT;
            goto cleanup;
        }
    }

cleanup:
    yyjson_doc_free(document);
    return status;
}

/* --- Capability ----------------------------------------------------------- */

/* A refutation method claims no proof soundness under any option, so the
   capability is constant. Options are still parsed here so a malformed node
   fails at scheduling time rather than mid-run. */
static ql_status QL_CALL diff_capability(
    const char *options_json, ql_proof_method_capability_v1 *capability,
    ql_error *error) {
    diff_instance probe;
    ql_status status;

    memset(&probe, 0, sizeof(probe));
    probe.allocator = *ql_default_allocator();
    probe.seed = QL_DIFF_DEFAULT_SEED;
    probe.tests = QL_DIFF_DEFAULT_TESTS;
    status = parse_options(&probe, options_json, error);
    if (status != QL_STATUS_OK) {
        return status;
    }
    capability->family = QL_PROOF_METHOD_FAMILY_CONCRETE_DIFFERENTIAL;
    capability->soundness_classes = QL_PROOF_SOUNDNESS_COUNTEREXAMPLE;
    capability->result_kinds =
        QL_PROOF_RESULT_COUNTEREXAMPLE | QL_PROOF_RESULT_UNKNOWN;
    capability->supported_relations = QL_PROOF_RELATION_ALL;
    capability->supported_ub_policies = QL_PROOF_UB_ALL;
    capability->supported_observations = QL_OBSERVE_ALL;
    capability->supported_memory_observations =
        QL_PROOF_MEMORY_MODE(QL_MEMORY_IGNORE) |
        QL_PROOF_MEMORY_MODE(QL_MEMORY_FINAL_REACHABLE_STATE) |
        QL_PROOF_MEMORY_MODE(QL_MEMORY_ORDERED_WRITES) |
        QL_PROOF_MEMORY_MODE(QL_MEMORY_FULL_TRACE);
    capability->supported_external_call_observations =
        QL_PROOF_EXTERNAL_CALL_MODE(QL_EXTERNAL_CALLS_IGNORE) |
        QL_PROOF_EXTERNAL_CALL_MODE(QL_EXTERNAL_CALLS_ORDERED_TRACE);
    capability->flags = QL_PROOF_CAPABILITY_PRECONDITIONS;
    ql_error_clear(error);
    return QL_STATUS_OK;
}

/* --- Lowering ------------------------------------------------------------- */

typedef struct lowered_side {
    ql_c_frontend_unit *unit;
    ql_c_lower_result *result;
    ql_ir *ir;
} lowered_side;

static void lowered_side_dispose(lowered_side *side) {
    ql_ir_release(side->ir);
    ql_c_lower_result_destroy(side->result);
    ql_c_frontend_unit_destroy(side->unit);
    memset(side, 0, sizeof(*side));
}

static ql_status lower_side(const ql_allocator *allocator, const char *source,
                            size_t source_size, const char *name,
                            size_t name_size, lowered_side *side,
                            uint32_t *supported, ql_error *error) {
    ql_c_function_view function;
    ql_c_lower_result_view_v1 view;
    ql_status status;

    memset(side, 0, sizeof(*side));
    *supported = 0u;
    status = ql_c_frontend_analyze(allocator, source, source_size, &side->unit,
                                   error);
    if (status != QL_STATUS_OK) {
        return status;
    }
    memset(&function, 0, sizeof(function));
    function.struct_size = sizeof(function);
    status = ql_c_frontend_select_function(side->unit, name, name_size,
                                           &function, error);
    if (status != QL_STATUS_OK) {
        return status;
    }
    status = ql_c_lower_selected_function(allocator, source, source_size,
                                          side->unit, &function, &side->result,
                                          error);
    if (status != QL_STATUS_OK) {
        return status;
    }
    memset(&view, 0, sizeof(view));
    view.struct_size = sizeof(view);
    status = ql_c_lower_result_get_view(side->result, &view, error);
    if (status != QL_STATUS_OK) {
        return status;
    }
    if (view.support != QL_C_LOWER_SUPPORTED || view.ir_artifact == NULL) {
        return QL_STATUS_OK;
    }
    status = ql_ir_open(allocator, view.ir_artifact, &side->ir, error);
    if (status != QL_STATUS_OK) {
        return status;
    }
    *supported = 1u;
    return QL_STATUS_OK;
}

/* --- Deterministic generator ---------------------------------------------- */

/* splitmix64. Chosen because its state is one word and its output sequence is
   fixed by the standard, so a recorded seed reproduces a recorded
   counterexample on any platform this library builds for. */
static uint64_t splitmix64(uint64_t *state) {
    uint64_t z;

    *state += UINT64_C(0x9e3779b97f4a7c15);
    z = *state;
    z = (z ^ (z >> 30)) * UINT64_C(0xbf58476d1ce4e5b9);
    z = (z ^ (z >> 27)) * UINT64_C(0x94d049bb133111eb);
    return z ^ (z >> 31);
}

static void bits_clear(uint8_t *bytes, uint32_t width) {
    memset(bytes, 0, ((size_t)width + 7u) / 8u);
}

static void bits_set(uint8_t *bytes, uint32_t position) {
    bytes[position / 8u] =
        (uint8_t)(bytes[position / 8u] | (uint8_t)(1u << (position % 8u)));
}

static void bits_fill_ones(uint8_t *bytes, uint32_t width) {
    uint32_t position;

    bits_clear(bytes, width);
    for (position = 0u; position < width; ++position) {
        bits_set(bytes, position);
    }
}

/* Writes the low `width` bits of `value`, discarding the rest. */
static void bits_from_u64(uint8_t *bytes, uint32_t width, uint64_t value) {
    uint32_t position;
    const uint32_t limit = width < 64u ? width : 64u;

    bits_clear(bytes, width);
    for (position = 0u; position < limit; ++position) {
        if (((value >> position) & UINT64_C(1)) != UINT64_C(0)) {
            bits_set(bytes, position);
        }
    }
}

static void bits_invert(uint8_t *bytes, uint32_t width) {
    uint32_t position;

    for (position = 0u; position < width; ++position) {
        bytes[position / 8u] =
            (uint8_t)(bytes[position / 8u] ^ (uint8_t)(1u << (position % 8u)));
    }
}

/* The boundary patterns, expressed on a width-bit value. Every pattern is
   defined for width one so a Boolean input degenerates cleanly. */
static void pattern_fill(uint32_t pattern, uint32_t width, uint8_t *bytes) {
    switch (pattern % QL_DIFF_PATTERN_COUNT) {
    case 0u: /* zero */
        bits_clear(bytes, width);
        return;
    case 1u: /* one */
        bits_from_u64(bytes, width, UINT64_C(1));
        return;
    case 2u: /* every bit set: unsigned maximum, signed -1 */
        bits_fill_ones(bytes, width);
        return;
    case 3u: /* sign bit only: signed minimum */
        bits_clear(bytes, width);
        bits_set(bytes, width - 1u);
        return;
    case 4u: /* every bit but the sign bit: signed maximum */
        bits_clear(bytes, width);
        bits_invert(bytes, width - 1u);
        return;
    case 5u: /* two */
        bits_from_u64(bytes, width, UINT64_C(2));
        return;
    case 6u: /* signed -2 */
        bits_fill_ones(bytes, width);
        bytes[0] = (uint8_t)(bytes[0] & 0xfeu);
        return;
    case 7u: /* alternating 0101 */
        bits_from_u64(bytes, width, UINT64_C(0x5555555555555555));
        if (width > 64u) {
            uint32_t position;
            for (position = 64u; position < width; ++position) {
                if ((position % 2u) == 0u) {
                    bits_set(bytes, position);
                }
            }
        }
        return;
    case 8u: /* alternating 1010 */
        bits_from_u64(bytes, width, UINT64_C(0xaaaaaaaaaaaaaaaa));
        if (width > 64u) {
            uint32_t position;
            for (position = 64u; position < width; ++position) {
                if ((position % 2u) == 1u) {
                    bits_set(bytes, position);
                }
            }
        }
        return;
    case 9u: /* signed minimum plus one */
        bits_clear(bytes, width);
        bits_set(bytes, width - 1u);
        bits_set(bytes, 0u);
        return;
    case 10u: /* 255: the byte boundary a narrowing conversion trips over */
        bits_from_u64(bytes, width, UINT64_C(255));
        return;
    default: /* the half-width bit: where a widening multiply overflows */
        bits_clear(bytes, width);
        bits_set(bytes, width / 2u);
        return;
    }
}

static void random_fill(uint64_t *state, uint32_t width, uint8_t *bytes) {
    uint32_t position = 0u;
    uint64_t chunk = 0u;

    bits_clear(bytes, width);
    while (position < width) {
        if ((position % 64u) == 0u) {
            chunk = splitmix64(state);
        }
        if (((chunk >> (position % 64u)) & UINT64_C(1)) != UINT64_C(0)) {
            bits_set(bytes, position);
        }
        ++position;
    }
}

/* --- Model text ----------------------------------------------------------- */

/* The generated tuple is written in the same syntax a solver model uses, so it
   is decoded and replayed by exactly the code that validates a solver's own
   witness. There is deliberately no second decoder to disagree with the
   first. */
typedef struct model_buffer {
    ql_allocator allocator;
    char *data;
    size_t size;
    size_t capacity;
} model_buffer;

static void model_buffer_dispose(model_buffer *buffer) {
    buffer->allocator.deallocate(buffer->allocator.user_data, buffer->data);
    buffer->data = NULL;
    buffer->size = 0u;
    buffer->capacity = 0u;
}

static int model_buffer_reserve(model_buffer *buffer, size_t extra) {
    size_t needed = buffer->size + extra + 1u;
    size_t capacity;
    char *grown;

    if (needed <= buffer->capacity) {
        return 1;
    }
    capacity = buffer->capacity == 0u ? 256u : buffer->capacity;
    while (capacity < needed) {
        if (capacity > (SIZE_MAX / 2u)) {
            return 0;
        }
        capacity *= 2u;
    }
    grown = buffer->allocator.reallocate(buffer->allocator.user_data,
                                         buffer->data, capacity);
    if (grown == NULL) {
        return 0;
    }
    buffer->data = grown;
    buffer->capacity = capacity;
    return 1;
}

static int model_buffer_append(model_buffer *buffer, const char *text,
                               size_t size) {
    if (!model_buffer_reserve(buffer, size)) {
        return 0;
    }
    memcpy(buffer->data + buffer->size, text, size);
    buffer->size += size;
    buffer->data[buffer->size] = '\0';
    return 1;
}

static int model_buffer_append_cstr(model_buffer *buffer, const char *text) {
    return model_buffer_append(buffer, text, strlen(text));
}

static int model_buffer_append_bits(model_buffer *buffer, const uint8_t *bytes,
                                    uint32_t width) {
    uint32_t index;

    if (!model_buffer_reserve(buffer, (size_t)width)) {
        return 0;
    }
    /* SMT-LIB binary literals are most significant bit first. */
    for (index = 0u; index < width; ++index) {
        const uint32_t position = width - 1u - index;
        const int bit =
            ((bytes[position / 8u] >> (position % 8u)) & 1u) != 0u;
        buffer->data[buffer->size + index] = bit ? '1' : '0';
    }
    buffer->size += (size_t)width;
    buffer->data[buffer->size] = '\0';
    return 1;
}

/* --- One trial ------------------------------------------------------------ */

typedef struct trial_state {
    ql_allocator allocator;
    const ql_problem *problem;
    const ql_product_query *query;
    const ql_ir *left_ir;
    const ql_ir *right_ir;
    size_t input_count;
} trial_state;

/* Fills `buffer` with the model text for one generated tuple. `round` selects
   the strategy: the first QL_DIFF_PATTERN_COUNT rounds set every input to the
   same boundary pattern, and later rounds mix boundary patterns with random
   bits per input. */
static ql_status build_tuple(const trial_state *state, uint64_t round,
                             uint64_t *random_state, model_buffer *buffer,
                             ql_error *error) {
    size_t index;

    buffer->size = 0u;
    if (!model_buffer_append_cstr(buffer, "(")) {
        ql_error_set(error, QL_STATUS_OUT_OF_MEMORY, NULL);
        return QL_STATUS_OUT_OF_MEMORY;
    }
    for (index = 0u; index < state->input_count; ++index) {
        uint8_t bytes[QL_REPLAY_MAX_INPUT_BYTES];
        char width_text[32];
        ql_product_input_v1 input;
        uint32_t width;
        int written;
        ql_status status;

        status = ql_product_query_input_at(state->query, index, &input, error);
        if (status != QL_STATUS_OK) {
            return status;
        }
        width = input.kind == QL_SOURCE_TYPE_BOOL ? 1u : input.bit_width;
        if (width == 0u || width > QL_REPLAY_MAX_INPUT_BYTES * 8u) {
            ql_error_set(error, QL_STATUS_TYPE_MISMATCH,
                         "input '%s' has a width this generator cannot produce",
                         input.symbol != NULL ? input.symbol : "");
            return QL_STATUS_TYPE_MISMATCH;
        }
        memset(bytes, 0, sizeof(bytes));
        if (round < (uint64_t)QL_DIFF_PATTERN_COUNT) {
            pattern_fill((uint32_t)round, width, bytes);
        } else {
            const uint64_t draw = splitmix64(random_state);
            if ((draw & UINT64_C(3)) == UINT64_C(0)) {
                pattern_fill((uint32_t)((draw >> 2) %
                                        (uint64_t)QL_DIFF_PATTERN_COUNT),
                             width, bytes);
            } else {
                random_fill(random_state, width, bytes);
            }
        }

        if (!model_buffer_append_cstr(buffer, "(define-fun ") ||
            !model_buffer_append_cstr(
                buffer, input.symbol != NULL ? input.symbol : "")) {
            ql_error_set(error, QL_STATUS_OUT_OF_MEMORY, NULL);
            return QL_STATUS_OUT_OF_MEMORY;
        }
        if (input.kind == QL_SOURCE_TYPE_BOOL) {
            const int bit = (bytes[0] & 1u) != 0u;
            if (!model_buffer_append_cstr(buffer, " () Bool ") ||
                !model_buffer_append_cstr(buffer, bit ? "true" : "false") ||
                !model_buffer_append_cstr(buffer, ")")) {
                ql_error_set(error, QL_STATUS_OUT_OF_MEMORY, NULL);
                return QL_STATUS_OUT_OF_MEMORY;
            }
            continue;
        }
        written = snprintf(width_text, sizeof(width_text), " () (_ BitVec %u) #b",
                           (unsigned)width);
        if (written < 0 || (size_t)written >= sizeof(width_text)) {
            ql_error_set(error, QL_STATUS_INTERNAL_ERROR,
                         "could not format a bit-vector sort");
            return QL_STATUS_INTERNAL_ERROR;
        }
        if (!model_buffer_append(buffer, width_text, (size_t)written) ||
            !model_buffer_append_bits(buffer, bytes, width) ||
            !model_buffer_append_cstr(buffer, ")")) {
            ql_error_set(error, QL_STATUS_OUT_OF_MEMORY, NULL);
            return QL_STATUS_OUT_OF_MEMORY;
        }
    }
    if (!model_buffer_append_cstr(buffer, ")")) {
        ql_error_set(error, QL_STATUS_OUT_OF_MEMORY, NULL);
        return QL_STATUS_OUT_OF_MEMORY;
    }
    return QL_STATUS_OK;
}

/* --- Search --------------------------------------------------------------- */

static ql_status search(const diff_instance *instance,
                        const ql_run_context_v1 *context,
                        const trial_state *state, diff_decision *decision,
                        ql_artifact **counterexample, ql_error *error) {
    model_buffer buffer;
    uint64_t random_state = instance->seed;
    uint64_t round;
    ql_status status = QL_STATUS_OK;

    *counterexample = NULL;
    memset(&buffer, 0, sizeof(buffer));
    buffer.allocator = instance->allocator;

    for (round = 0u; round < instance->tests; ++round) {
        ql_artifact *model = NULL;
        ql_replay_witness *witness = NULL;
        ql_replay_result_v1 replay;

        if (context != NULL && context->is_cancelled != NULL &&
            context->is_cancelled(context->cancel_state) != 0u) {
            set_diagnostic(decision,
                           "the differential search was cancelled before it finished");
            goto finish;
        }
        status = build_tuple(state, round, &random_state, &buffer, error);
        if (status != QL_STATUS_OK) {
            goto finish;
        }
        status = ql_artifact_create(&state->allocator,
                                    QL_ARTIFACT_KIND_SOLVER_MODEL,
                                    QL_SOLVER_ARTIFACT_SCHEMA_VERSION,
                                    buffer.data,
                                    buffer.size, &model, error);
        if (status != QL_STATUS_OK) {
            goto finish;
        }
        status = ql_replay_decode_model(&state->allocator, state->query, model,
                                        &witness, error);
        ql_artifact_release(model);
        if (status != QL_STATUS_OK) {
            /* The generator produced something the shared decoder rejects.
               That is a defect in this method, not a fact about the two
               functions, and it stops the run rather than being counted as a
               passing test. */
            QL_LOGE(QL_DIFF_CATEGORY,
                    "generated tuple %llu could not be decoded: %s",
                    (unsigned long long)round, error->message);
            goto finish;
        }
        ++decision->tests_executed;
        memset(&replay, 0, sizeof(replay));
        replay.struct_size = sizeof(replay);
        status = ql_replay_execute(&state->allocator, state->problem,
                                   state->query, state->left_ir,
                                   state->right_ir, witness, &replay, error);
        if (status != QL_STATUS_OK) {
            ql_replay_witness_destroy(witness);
            goto finish;
        }
        if (replay.conclusive == 0u) {
            if (replay.precondition_evaluated != 0u &&
                replay.precondition_holds == 0u) {
                ++decision->tests_precondition_rejected;
            }
            ql_replay_witness_destroy(witness);
            continue;
        }
        ++decision->tests_conclusive;
        if (replay.violated == 0u) {
            ql_replay_witness_destroy(witness);
            continue;
        }
        /* The mismatch was produced by running both functions concretely, so
           it is replay-confirmed by construction. */
        status = ql_replay_counterexample_artifact_create(
            &state->allocator, state->query, witness, &replay, counterexample,
            error);
        ql_replay_witness_destroy(witness);
        if (status != QL_STATUS_OK) {
            goto finish;
        }
        decision->verdict = QL_VERDICT_COUNTEREXAMPLE;
        decision->evidence_class = QL_EVIDENCE_COUNTEREXAMPLE;
        decision->replay_confirmed = 1u;
        set_diagnostic(decision,
                       "concrete execution of both functions broke the declared relation on a generated input");
        goto finish;
    }

    if (decision->tests_conclusive == 0u) {
        set_diagnostic(decision,
                       "no generated input reached a decidable comparison, so the search says nothing about the two functions");
    } else {
        /* Sampling exhausts no bound, so this is UNKNOWN and never
           BOUNDED_CLEAN. */
        set_diagnostic(decision,
                       "no generated input broke the relation; sampling exhausts no bound, so this is not a clean bounded result");
    }

finish:
    model_buffer_dispose(&buffer);
    return status;
}

/* --- Outcome serialization ------------------------------------------------ */

static int add_digest(yyjson_mut_doc *document, yyjson_mut_val *object,
                      const char *key, const ql_digest *digest) {
    char hex[QL_DIGEST_HEX_SIZE];
    ql_digest_hex(digest, hex);
    return yyjson_mut_obj_add_strcpy(document, object, key, hex);
}

static ql_status compute_cache_key(const ql_digest *problem_digest,
                                   const diff_instance *instance,
                                   const ql_product_query_view_v1 *query,
                                   ql_digest *cache_key, ql_error *error) {
    /* The generator is deterministic, so the seed and the test count are part
       of the identity: a different sample is a different search. */
    char identity[512];
    char prefix_hex[QL_DIGEST_HEX_SIZE];
    char violation_hex[QL_DIGEST_HEX_SIZE];
    ql_cache_key_input_v1 input;
    int written;

    ql_digest_hex(&query->prefix_digest, prefix_hex);
    ql_digest_hex(&query->violation_digest, violation_hex);
    written = snprintf(
        identity, sizeof(identity),
        "{\"seed\":%llu,\"tests\":%llu,\"generator\":\"splitmix64+boundary/%u\",\"prefix\":\"%s\",\"violation\":\"%s\"}",
        (unsigned long long)instance->seed,
        (unsigned long long)instance->tests, (unsigned)QL_DIFF_PATTERN_COUNT,
        prefix_hex, violation_hex);
    if (written < 0 || (size_t)written >= sizeof(identity)) {
        ql_error_set(error, QL_STATUS_INTERNAL_ERROR,
                     "could not format the cache identity");
        return QL_STATUS_INTERNAL_ERROR;
    }
    memset(&input, 0, sizeof(input));
    input.struct_size = sizeof(input);
    input.artifact_digest = *problem_digest;
    input.semantic_problem_digest = *problem_digest;
    input.method_name = QL_DIFF_METHOD_NAME;
    input.method_version = QL_DIFF_METHOD_VERSION;
    input.canonical_options = identity;
    input.canonical_options_size = (size_t)written;
    return ql_cache_key_compute(&input, cache_key, error);
}

static ql_status build_outcome(const ql_allocator *allocator,
                               const diff_instance *instance,
                               const ql_problem_view_v2 *problem_view,
                               const ql_product_query_view_v1 *query_view,
                               const diff_decision *decision,
                               const ql_artifact *counterexample,
                               ql_artifact **output, ql_error *error) {
    ql_allocator allocator_copy = *allocator;
    yyjson_alc json_allocator = diff_json_allocator(&allocator_copy);
    yyjson_mut_doc *document = NULL;
    yyjson_mut_val *root;
    yyjson_mut_val *search_object;
    yyjson_mut_val *trust_object;
    yyjson_write_err write_error;
    ql_artifact_view counterexample_view;
    ql_digest cache_key;
    ql_digest empty_digest;
    char *json = NULL;
    size_t json_size = 0u;
    ql_status status;

    memset(&empty_digest, 0, sizeof(empty_digest));
    memset(&counterexample_view, 0, sizeof(counterexample_view));
    counterexample_view.struct_size = sizeof(counterexample_view);
    if (counterexample != NULL) {
        status = ql_artifact_get_view(counterexample, &counterexample_view,
                                      error);
        if (status != QL_STATUS_OK) {
            return status;
        }
    }
    status = compute_cache_key(&problem_view->artifact_digest, instance,
                               query_view, &cache_key, error);
    if (status != QL_STATUS_OK) {
        return status;
    }

    document = yyjson_mut_doc_new(&json_allocator);
    root = document != NULL ? yyjson_mut_obj(document) : NULL;
    search_object = document != NULL ? yyjson_mut_obj(document) : NULL;
    trust_object = document != NULL ? yyjson_mut_obj(document) : NULL;
    if (document == NULL || root == NULL || search_object == NULL ||
        trust_object == NULL ||
        !yyjson_mut_obj_add_str(document, root, "kind",
                                QL_ARTIFACT_KIND_OUTCOME) ||
        !yyjson_mut_obj_add_uint(document, root, "schema_version",
                                 QL_DIFF_OUTCOME_SCHEMA_VERSION) ||
        !yyjson_mut_obj_add_str(document, root, "method",
                                QL_DIFF_METHOD_NAME) ||
        !yyjson_mut_obj_add_str(document, root, "method_version",
                                QL_DIFF_METHOD_VERSION) ||
        !yyjson_mut_obj_add_str(document, root, "verdict",
                                ql_verdict_string(decision->verdict)) ||
        !yyjson_mut_obj_add_str(
            document, root, "evidence_class",
            ql_evidence_class_string(decision->evidence_class)) ||
        !add_digest(document, root, "problem_digest",
                    &problem_view->artifact_digest) ||
        !add_digest(document, root, "cache_key", &cache_key) ||
        !yyjson_mut_obj_add_uint(document, root, "relation",
                                 (uint64_t)query_view->relation) ||
        !yyjson_mut_obj_add_uint(document, root, "ub_policy",
                                 (uint64_t)query_view->ub_policy) ||
        !yyjson_mut_obj_add_uint(document, root, "observations",
                                 query_view->covered_observations) ||
        !yyjson_mut_obj_add_strcpy(document, root, "diagnostic",
                                   decision->diagnostic)) {
        status = QL_STATUS_OUT_OF_MEMORY;
        ql_error_set(error, status, NULL);
        goto cleanup;
    }
    if (!yyjson_mut_obj_add_str(document, search_object, "generator",
                                "splitmix64 over boundary patterns and uniform bits") ||
        !yyjson_mut_obj_add_uint(document, search_object, "seed",
                                 instance->seed) ||
        !yyjson_mut_obj_add_uint(document, search_object, "requested_tests",
                                 instance->tests) ||
        !yyjson_mut_obj_add_uint(document, search_object, "tests_executed",
                                 decision->tests_executed) ||
        !yyjson_mut_obj_add_uint(document, search_object, "tests_conclusive",
                                 decision->tests_conclusive) ||
        !yyjson_mut_obj_add_uint(document, search_object,
                                 "tests_precondition_rejected",
                                 decision->tests_precondition_rejected) ||
        !add_digest(document, search_object, "prefix_digest",
                    &query_view->prefix_digest) ||
        !yyjson_mut_obj_add_val(document, root, "search", search_object)) {
        status = QL_STATUS_OUT_OF_MEMORY;
        ql_error_set(error, status, NULL);
        goto cleanup;
    }
    /* A refutation method states its limits in the envelope rather than
       leaving a reader to infer them from a passing test count. */
    if (!yyjson_mut_obj_add_bool(document, trust_object, "checked_proof", 0) ||
        !yyjson_mut_obj_add_bool(document, trust_object, "replay_confirmed",
                                 decision->replay_confirmed != 0u) ||
        !yyjson_mut_obj_add_str(
            document, trust_object, "basis",
            decision->verdict == QL_VERDICT_COUNTEREXAMPLE
                ? "both functions were executed concretely on the recorded input and the declared relation did not hold"
                : "sampling found no mismatch; this method exhausts no bound and never claims a proof or a clean bounded result") ||
        !yyjson_mut_obj_add_val(document, root, "trust", trust_object)) {
        status = QL_STATUS_OUT_OF_MEMORY;
        ql_error_set(error, status, NULL);
        goto cleanup;
    }
    if (counterexample != NULL) {
        if (!add_digest(document, root, "counterexample_digest",
                        &counterexample_view.digest) ||
            !yyjson_mut_obj_add_strncpy(document, root, "counterexample",
                                        (const char *)counterexample_view.data,
                                        counterexample_view.size)) {
            status = QL_STATUS_OUT_OF_MEMORY;
            ql_error_set(error, status, NULL);
            goto cleanup;
        }
    } else if (!add_digest(document, root, "counterexample_digest",
                           &empty_digest) ||
               !yyjson_mut_obj_add_null(document, root, "counterexample")) {
        status = QL_STATUS_OUT_OF_MEMORY;
        ql_error_set(error, status, NULL);
        goto cleanup;
    }

    yyjson_mut_doc_set_root(document, root);
    json = yyjson_mut_write_opts(document, 0u, &json_allocator, &json_size,
                                 &write_error);
    if (json == NULL) {
        status = write_error.code == YYJSON_WRITE_ERROR_MEMORY_ALLOCATION
                     ? QL_STATUS_OUT_OF_MEMORY
                     : QL_STATUS_PARSE_ERROR;
        ql_error_set(error, status, "cannot serialize the outcome JSON: %s",
                     write_error.msg != NULL ? write_error.msg
                                             : "JSON write error");
        goto cleanup;
    }
    status = ql_artifact_create(allocator, QL_ARTIFACT_KIND_OUTCOME,
                                QL_DIFF_OUTCOME_SCHEMA_VERSION, json,
                                json_size, output, error);

cleanup:
    if (json != NULL) {
        json_allocator.free(json_allocator.ctx, json);
    }
    if (document != NULL) {
        yyjson_mut_doc_free(document);
    }
    return status;
}

/* --- Method callbacks ----------------------------------------------------- */

static ql_status QL_CALL diff_create(const ql_host_v1 *host,
                                     const char *options_json, void **instance,
                                     ql_error *error) {
    diff_instance *created;
    ql_allocator allocator;
    ql_status status;

    if (host == NULL || instance == NULL) {
        ql_error_set(error, QL_STATUS_INVALID_ARGUMENT,
                     "host and instance output are required");
        return QL_STATUS_INVALID_ARGUMENT;
    }
    *instance = NULL;
    allocator = host->allocator;
    if (!ql_allocator_is_valid(&allocator)) {
        allocator = *ql_default_allocator();
    }
    created = allocator.allocate(allocator.user_data, sizeof(*created));
    if (created == NULL) {
        ql_error_set(error, QL_STATUS_OUT_OF_MEMORY, NULL);
        return QL_STATUS_OUT_OF_MEMORY;
    }
    memset(created, 0, sizeof(*created));
    created->allocator = allocator;
    created->seed = QL_DIFF_DEFAULT_SEED;
    created->tests = QL_DIFF_DEFAULT_TESTS;
    status = parse_options(created, options_json, error);
    if (status != QL_STATUS_OK) {
        allocator.deallocate(allocator.user_data, created);
        return status;
    }
    *instance = created;
    ql_error_clear(error);
    return QL_STATUS_OK;
}

static void QL_CALL diff_destroy(void *instance) {
    diff_instance *owned = (diff_instance *)instance;
    ql_allocator allocator;

    if (owned == NULL) {
        return;
    }
    allocator = owned->allocator;
    allocator.deallocate(allocator.user_data, owned);
}

static ql_status QL_CALL diff_validate(void *instance,
                                       ql_artifact *const *inputs,
                                       size_t input_count, ql_error *error) {
    ql_artifact_view view;
    ql_status status;

    (void)instance;
    if (inputs == NULL || input_count != 1u || inputs[0] == NULL) {
        ql_error_set(error, QL_STATUS_INVALID_ARGUMENT,
                     "%s consumes exactly one quodlibet.problem artifact",
                     QL_DIFF_METHOD_NAME);
        return QL_STATUS_INVALID_ARGUMENT;
    }
    memset(&view, 0, sizeof(view));
    view.struct_size = sizeof(view);
    status = ql_artifact_get_view(inputs[0], &view, error);
    if (status != QL_STATUS_OK) {
        return status;
    }
    if (strcmp(view.kind, QL_ARTIFACT_KIND_PROBLEM) != 0) {
        ql_error_set(error, QL_STATUS_TYPE_MISMATCH,
                     "%s input is not a quodlibet.problem",
                     QL_DIFF_METHOD_NAME);
        return QL_STATUS_TYPE_MISMATCH;
    }
    /* Without the recorded argument correspondence there is no stated relation
       between the two input lists, so there is nothing to feed the same tuple
       into. */
    if (view.schema_version != QL_PROBLEM_SCHEMA_VERSION_2) {
        ql_error_set(error, QL_STATUS_TYPE_MISMATCH,
                     "%s requires problem schema v2; schema v%u records no argument correspondence",
                     QL_DIFF_METHOD_NAME, view.schema_version);
        return QL_STATUS_TYPE_MISMATCH;
    }
    ql_error_clear(error);
    return QL_STATUS_OK;
}

static ql_status QL_CALL diff_run(void *instance,
                                  const ql_run_context_v1 *context,
                                  ql_artifact *const *inputs,
                                  size_t input_count, ql_artifact **output,
                                  ql_error *error) {
    diff_instance *owned = (diff_instance *)instance;
    const ql_allocator *allocator;
    ql_problem *problem = NULL;
    ql_problem_view_v2 problem_view;
    ql_product_query *query = NULL;
    ql_product_query_view_v1 query_view;
    ql_artifact *counterexample = NULL;
    lowered_side left;
    lowered_side right;
    diff_decision decision;
    trial_state state;
    uint32_t left_supported = 0u;
    uint32_t right_supported = 0u;
    ql_status status;

    memset(&left, 0, sizeof(left));
    memset(&right, 0, sizeof(right));
    memset(&decision, 0, sizeof(decision));
    memset(&state, 0, sizeof(state));
    decision.verdict = QL_VERDICT_UNKNOWN;
    decision.evidence_class = QL_EVIDENCE_UNKNOWN;
    if (owned == NULL || context == NULL || output == NULL) {
        ql_error_set(error, QL_STATUS_INVALID_ARGUMENT,
                     "instance, run context, and output are required");
        return QL_STATUS_INVALID_ARGUMENT;
    }
    *output = NULL;
    status = diff_validate(instance, inputs, input_count, error);
    if (status != QL_STATUS_OK) {
        return status;
    }
    allocator = &owned->allocator;
    status = ql_problem_open(allocator, inputs[0], &problem, error);
    if (status != QL_STATUS_OK) {
        return status;
    }
    memset(&problem_view, 0, sizeof(problem_view));
    problem_view.struct_size = sizeof(problem_view);
    status = ql_problem_get_view_v2(problem, &problem_view, error);
    if (status != QL_STATUS_OK) {
        goto cleanup;
    }

    status = lower_side(allocator, problem_view.left_source,
                        problem_view.left_source_size,
                        problem_view.left_function_name,
                        problem_view.left_function_name_size, &left,
                        &left_supported, error);
    if (status == QL_STATUS_OK) {
        status = lower_side(allocator, problem_view.right_source,
                            problem_view.right_source_size,
                            problem_view.right_function_name,
                            problem_view.right_function_name_size, &right,
                            &right_supported, error);
    }
    if (status != QL_STATUS_OK) {
        goto cleanup;
    }
    memset(&query_view, 0, sizeof(query_view));
    query_view.struct_size = sizeof(query_view);
    if (left_supported == 0u || right_supported == 0u) {
        set_diagnostic(&decision,
                       "the semantic C lowering does not support one of the two functions, so neither could be executed");
        query_view.relation = problem_view.contract.relation;
        query_view.ub_policy = problem_view.contract.ub_policy;
        query_view.covered_observations = problem_view.contract.observations;
        status = build_outcome(allocator, owned, &problem_view, &query_view,
                               &decision, NULL, output, error);
        goto cleanup;
    }

    /* The product query is built even though nothing is solved. It is what
       states the shared input list, the argument correspondence, and the exact
       observation axes, and it is what refuses territory this slice does not
       model instead of narrowing it. */
    status = ql_product_query_build(allocator, problem, left.ir, right.ir,
                                    &query, error);
    if (status == QL_STATUS_TYPE_MISMATCH) {
        set_diagnostic(&decision, error->message);
        query_view.relation = problem_view.contract.relation;
        query_view.ub_policy = problem_view.contract.ub_policy;
        query_view.covered_observations = problem_view.contract.observations;
        status = build_outcome(allocator, owned, &problem_view, &query_view,
                               &decision, NULL, output, error);
        goto cleanup;
    }
    if (status != QL_STATUS_OK) {
        goto cleanup;
    }
    status = ql_product_query_get_view(query, &query_view, error);
    if (status != QL_STATUS_OK) {
        goto cleanup;
    }

    state.allocator = *allocator;
    state.problem = problem;
    state.query = query;
    state.left_ir = left.ir;
    state.right_ir = right.ir;
    state.input_count = query_view.input_count;
    status = search(owned, context, &state, &decision, &counterexample, error);
    if (status != QL_STATUS_OK) {
        goto cleanup;
    }
    status = build_outcome(allocator, owned, &problem_view, &query_view,
                           &decision, counterexample, output, error);

cleanup:
    ql_artifact_release(counterexample);
    ql_product_query_destroy(query);
    lowered_side_dispose(&left);
    lowered_side_dispose(&right);
    ql_problem_release(problem);
    if (status == QL_STATUS_OK) {
        ql_error_clear(error);
    }
    return status;
}

static const ql_method_v1 diff_method = {
    sizeof(ql_method_v1),
    QL_ABI_VERSION,
    QL_DIFF_METHOD_NAME,
    "Concrete differential refutation of two loop-free scalar C functions",
    QL_ARTIFACT_KIND_OUTCOME,
    QL_METHOD_DETERMINISTIC | QL_METHOD_COUNTEREXAMPLE_PRODUCER |
        QL_METHOD_CACHEABLE,
    1u,
    1u,
    diff_create,
    diff_validate,
    diff_run,
    diff_destroy,
    { NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL }
};

static const ql_proof_method_v1 diff_descriptor = {
    sizeof(ql_proof_method_v1),
    QL_ABI_VERSION,
    QL_PROOF_METHOD_FAMILY_CONCRETE_DIFFERENTIAL,
    &diff_method,
    diff_capability,
    { NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL }
};

const ql_method_v1 *QL_CALL ql_diff_method(void) {
    return &diff_method;
}

const ql_proof_method_v1 *QL_CALL ql_diff_proof_method(void) {
    return &diff_descriptor;
}

ql_status QL_CALL ql_register_diff_method(ql_registry *registry,
                                          ql_error *error) {
    ql_status status = ql_registry_register(registry, &diff_method, error);
    if (status != QL_STATUS_OK) {
        return status;
    }
    return ql_registry_register_proof_method(registry, &diff_descriptor,
                                             error);
}

/* --- Outcome reading ------------------------------------------------------ */

static int read_digest_field(yyjson_val *object, const char *key,
                             ql_digest *digest) {
    yyjson_val *value = yyjson_obj_get(object, key);
    const char *text;
    size_t index;

    memset(digest, 0, sizeof(*digest));
    if (!yyjson_is_str(value) ||
        yyjson_get_len(value) != QL_DIGEST_HEX_SIZE - 1u) {
        return 0;
    }
    text = yyjson_get_str(value);
    for (index = 0u; index < QL_DIGEST_SIZE; ++index) {
        uint32_t byte = 0u;
        uint32_t nibble;
        for (nibble = 0u; nibble < 2u; ++nibble) {
            const char ch = text[index * 2u + nibble];
            uint32_t digit;
            if (ch >= '0' && ch <= '9') {
                digit = (uint32_t)(ch - '0');
            } else if (ch >= 'a' && ch <= 'f') {
                digit = (uint32_t)(ch - 'a' + 10);
            } else {
                return 0;
            }
            byte = (byte << 4) | digit;
        }
        digest->bytes[index] = (uint8_t)byte;
    }
    return 1;
}

static ql_verdict verdict_parse(const char *text) {
    ql_verdict verdict;
    for (verdict = QL_VERDICT_UNKNOWN; verdict <= QL_VERDICT_BOUNDED_CLEAN;
         ++verdict) {
        if (strcmp(ql_verdict_string(verdict), text) == 0) {
            return verdict;
        }
    }
    return QL_VERDICT_UNKNOWN;
}

static ql_evidence_class evidence_parse(const char *text) {
    ql_evidence_class evidence;
    for (evidence = QL_EVIDENCE_PROOF; evidence <= QL_EVIDENCE_UNKNOWN;
         ++evidence) {
        if (strcmp(ql_evidence_class_string(evidence), text) == 0) {
            return evidence;
        }
    }
    return QL_EVIDENCE_UNKNOWN;
}

static ql_status open_outcome_document(const ql_artifact *artifact,
                                       ql_artifact_view *view,
                                       yyjson_doc **document,
                                       ql_error *error) {
    yyjson_read_err read_error;
    ql_status status;

    memset(view, 0, sizeof(*view));
    view->struct_size = sizeof(*view);
    status = ql_artifact_get_view(artifact, view, error);
    if (status != QL_STATUS_OK) {
        return status;
    }
    if (strcmp(view->kind, QL_ARTIFACT_KIND_OUTCOME) != 0 ||
        view->schema_version != QL_DIFF_OUTCOME_SCHEMA_VERSION) {
        ql_error_set(error, QL_STATUS_TYPE_MISMATCH,
                     "artifact is not a schema v1 quodlibet.outcome");
        return QL_STATUS_TYPE_MISMATCH;
    }
    *document = yyjson_read_opts((char *)(uintptr_t)view->data, view->size, 0u,
                                 NULL, &read_error);
    if (*document == NULL) {
        ql_error_set(error, QL_STATUS_PARSE_ERROR,
                     "invalid outcome JSON at byte %zu: %s", read_error.pos,
                     read_error.msg != NULL ? read_error.msg : "parse error");
        return QL_STATUS_PARSE_ERROR;
    }
    return QL_STATUS_OK;
}

ql_status QL_CALL ql_diff_outcome_read(const ql_artifact *artifact,
                                       ql_diff_outcome_view_v1 *view,
                                       ql_error *error) {
    ql_artifact_view artifact_view;
    yyjson_doc *document = NULL;
    yyjson_val *root;
    yyjson_val *search_object;
    yyjson_val *trust;
    const char *text;
    ql_status status;

    if (artifact == NULL || view == NULL) {
        ql_error_set(error, QL_STATUS_INVALID_ARGUMENT,
                     "outcome artifact and view are required");
        return QL_STATUS_INVALID_ARGUMENT;
    }
    if (view->struct_size != 0u && view->struct_size < sizeof(*view)) {
        ql_error_set(error, QL_STATUS_ABI_MISMATCH,
                     "outcome view structure is too small");
        return QL_STATUS_ABI_MISMATCH;
    }
    status = open_outcome_document(artifact, &artifact_view, &document, error);
    if (status != QL_STATUS_OK) {
        return status;
    }
    memset(view, 0, sizeof(*view));
    view->struct_size = sizeof(*view);
    view->schema_version = QL_DIFF_OUTCOME_SCHEMA_VERSION;
    root = yyjson_doc_get_root(document);
    search_object = yyjson_obj_get(root, "search");
    trust = yyjson_obj_get(root, "trust");
    if (!yyjson_is_obj(root) || !yyjson_is_obj(search_object) ||
        !yyjson_is_obj(trust)) {
        ql_error_set(error, QL_STATUS_SCHEMA_MISMATCH,
                     "outcome JSON does not match schema version 1");
        status = QL_STATUS_SCHEMA_MISMATCH;
        goto cleanup;
    }
    text = yyjson_get_str(yyjson_obj_get(root, "method"));
    if (text == NULL || strcmp(text, QL_DIFF_METHOD_NAME) != 0) {
        ql_error_set(error, QL_STATUS_TYPE_MISMATCH,
                     "outcome was not produced by %s", QL_DIFF_METHOD_NAME);
        status = QL_STATUS_TYPE_MISMATCH;
        goto cleanup;
    }
    text = yyjson_get_str(yyjson_obj_get(root, "verdict"));
    view->verdict = text != NULL ? verdict_parse(text) : QL_VERDICT_UNKNOWN;
    text = yyjson_get_str(yyjson_obj_get(root, "evidence_class"));
    view->evidence_class =
        text != NULL ? evidence_parse(text) : QL_EVIDENCE_UNKNOWN;
    view->checked_proof =
        yyjson_get_bool(yyjson_obj_get(trust, "checked_proof")) ? 1u : 0u;
    view->replay_confirmed =
        yyjson_get_bool(yyjson_obj_get(trust, "replay_confirmed")) ? 1u : 0u;
    view->seed = yyjson_get_uint(yyjson_obj_get(search_object, "seed"));
    view->tests_executed =
        yyjson_get_uint(yyjson_obj_get(search_object, "tests_executed"));
    view->tests_conclusive =
        yyjson_get_uint(yyjson_obj_get(search_object, "tests_conclusive"));
    view->tests_precondition_rejected = yyjson_get_uint(
        yyjson_obj_get(search_object, "tests_precondition_rejected"));
    (void)read_digest_field(root, "problem_digest", &view->problem_digest);
    (void)read_digest_field(root, "cache_key", &view->cache_key);
    (void)read_digest_field(root, "counterexample_digest",
                            &view->counterexample_digest);
    text = yyjson_get_str(yyjson_obj_get(root, "diagnostic"));
    if (text != NULL) {
        size_t length = strlen(text);
        if (length >= sizeof(view->diagnostic)) {
            length = sizeof(view->diagnostic) - 1u;
        }
        memcpy(view->diagnostic, text, length);
        view->diagnostic[length] = '\0';
    }
    ql_error_clear(error);

cleanup:
    yyjson_doc_free(document);
    return status;
}

ql_status QL_CALL ql_diff_outcome_counterexample(const ql_allocator *allocator,
                                                 const ql_artifact *artifact,
                                                 ql_artifact **output,
                                                 ql_error *error) {
    ql_artifact_view artifact_view;
    yyjson_doc *document = NULL;
    yyjson_val *value;
    ql_status status;

    if (artifact == NULL || output == NULL) {
        ql_error_set(error, QL_STATUS_INVALID_ARGUMENT,
                     "outcome artifact and output are required");
        return QL_STATUS_INVALID_ARGUMENT;
    }
    *output = NULL;
    status = open_outcome_document(artifact, &artifact_view, &document, error);
    if (status != QL_STATUS_OK) {
        return status;
    }
    value = yyjson_obj_get(yyjson_doc_get_root(document), "counterexample");
    if (yyjson_is_str(value)) {
        status = ql_artifact_create(allocator, QL_ARTIFACT_KIND_COUNTEREXAMPLE,
                                    QL_REPLAY_SCHEMA_VERSION,
                                    yyjson_get_str(value),
                                    yyjson_get_len(value), output, error);
    } else {
        ql_error_clear(error);
    }
    yyjson_doc_free(document);
    return status;
}
