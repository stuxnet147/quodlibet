#include "quodlibet/proof_chcpdr.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "quodlibet/c_lower.h"
#include "quodlibet/log.h"
#include "quodlibet/problem.h"
#include "quodlibet/solver.h"

#include "loop_proof.h"
#include "yyjson.h"

#define QL_CHCPDR_CATEGORY "proof.chc-pdr"
#define QL_CHCPDR_MAX_STATE_VARIABLES 64u
#define QL_CHCPDR_MAX_CUBE_LITERALS QL_CHCPDR_MAX_STATE_VARIABLES

typedef enum chcpdr_promotion {
    QL_CHCPDR_PROMOTION_NONE = 0,
    QL_CHCPDR_PROMOTION_TRUSTED_BACKEND
} chcpdr_promotion;

typedef struct chcpdr_instance {
    ql_allocator allocator;
    chcpdr_promotion unsat_promotion;
    uint64_t timeout_ms;
    uint64_t max_frames;
    uint64_t max_lemmas;
    uint64_t max_queries;
    char *solver_options;
} chcpdr_instance;

typedef struct chcpdr_decision {
    ql_verdict verdict;
    ql_evidence_class evidence_class;
    uint32_t invariant_verified;
    uint32_t frames_used;
    uint64_t lemma_count;
    uint64_t invariant_lemma_count;
    uint64_t queries_used;
    const char *backend_name;
    const char *backend_version;
    ql_digest backend_binary_digest;
    char diagnostic[QL_ERROR_MESSAGE_CAPACITY];
} chcpdr_decision;

/* --- Text buffer ---------------------------------------------------------- */

typedef struct chcpdr_buffer {
    ql_allocator allocator;
    char *data;
    size_t size;
    size_t capacity;
} chcpdr_buffer;

static void buffer_init(chcpdr_buffer *buffer, const ql_allocator *allocator) {
    memset(buffer, 0, sizeof(*buffer));
    buffer->allocator = *allocator;
}

static void buffer_dispose(chcpdr_buffer *buffer) {
    buffer->allocator.deallocate(buffer->allocator.user_data, buffer->data);
    buffer->data = NULL;
    buffer->size = 0u;
    buffer->capacity = 0u;
}

static ql_status buffer_add_n(chcpdr_buffer *buffer, const char *text,
                              size_t size, ql_error *error) {
    if (buffer->size + size + 1u > buffer->capacity) {
        size_t capacity = buffer->capacity == 0u ? 512u : buffer->capacity;
        char *grown;
        while (capacity < buffer->size + size + 1u) {
            if (capacity > (SIZE_MAX / 2u)) {
                ql_error_set(error, QL_STATUS_OUT_OF_MEMORY, NULL);
                return QL_STATUS_OUT_OF_MEMORY;
            }
            capacity *= 2u;
        }
        grown = buffer->allocator.reallocate(buffer->allocator.user_data,
                                             buffer->data, capacity);
        if (grown == NULL) {
            ql_error_set(error, QL_STATUS_OUT_OF_MEMORY, NULL);
            return QL_STATUS_OUT_OF_MEMORY;
        }
        buffer->data = grown;
        buffer->capacity = capacity;
    }
    memcpy(buffer->data + buffer->size, text, size);
    buffer->size += size;
    buffer->data[buffer->size] = '\0';
    return QL_STATUS_OK;
}

static ql_status buffer_add(chcpdr_buffer *buffer, const char *text,
                            ql_error *error) {
    return buffer_add_n(buffer, text, strlen(text), error);
}

static ql_status buffer_addf(chcpdr_buffer *buffer, ql_error *error,
                             const char *format, ...) {
    char scratch[512];
    va_list arguments;
    int written;

    va_start(arguments, format);
    written = vsnprintf(scratch, sizeof(scratch), format, arguments);
    va_end(arguments);
    if (written < 0 || (size_t)written >= sizeof(scratch)) {
        ql_error_set(error, QL_STATUS_INTERNAL_ERROR,
                     "a PDR formula fragment overflowed its scratch buffer");
        return QL_STATUS_INTERNAL_ERROR;
    }
    return buffer_add_n(buffer, scratch, (size_t)written, error);
}

/* memmem is not in C17 and not on every libc this repository builds
   against, so the byte search is spelled out once here. */
static const void *find_bytes(const void *haystack, size_t haystack_size,
                              const void *needle, size_t needle_size) {
    const char *text = (const char *)haystack;
    size_t index;

    if (needle_size == 0u || haystack_size < needle_size) {
        return NULL;
    }
    for (index = 0u; index + needle_size <= haystack_size; ++index) {
        if (memcmp(text + index, needle, needle_size) == 0) {
            return text + index;
        }
    }
    return NULL;
}

/* --- State inventory ------------------------------------------------------ */

typedef struct chcpdr_variable {
    uint32_t is_right;
    uint32_t phi;
    uint32_t is_bool;
    uint32_t width;
} chcpdr_variable;

static void variable_symbol(const chcpdr_variable *variable, uint32_t kind,
                            char *text, size_t capacity) {
    /* kind: 0 current, 1 next, 2 entry. */
    const char side = variable->is_right ? 'r' : 'l';
    if (kind == 0u) {
        (void)snprintf(text, capacity, "ql_lp_%c_0_%u", side, variable->phi);
    } else if (kind == 1u) {
        (void)snprintf(text, capacity, "ql_lp_%c_next_0_%u", side,
                       variable->phi);
    } else {
        (void)snprintf(text, capacity, "ql_lp_%c_entry_0_%u", side,
                       variable->phi);
    }
}

/* Reads the state inventory back out of the serialized prefix. The producer
   is in this repository and its naming is deterministic, so this is a closed
   grammar read of bytes another translation unit wrote -- the same posture
   the AIG blaster takes toward the SMT-LIB builder. */
static ql_status parse_state_inventory(const ql_artifact *prefix,
                                       chcpdr_variable *variables,
                                       size_t capacity, size_t *count,
                                       ql_error *error) {
    ql_artifact_view view;
    const char *text;
    size_t size;
    size_t cursor = 0u;
    ql_status status;

    *count = 0u;
    memset(&view, 0, sizeof(view));
    view.struct_size = sizeof(view);
    status = ql_artifact_get_view(prefix, &view, error);
    if (status != QL_STATUS_OK) {
        return status;
    }
    text = (const char *)view.data;
    size = view.size;
    while (cursor < size) {
        static const char lead[] = "(declare-fun ql_lp_";
        const char *found = NULL;
        size_t remaining = size - cursor;
        const char *scan = text + cursor;

        found = (const char *)find_bytes(scan, remaining, lead,
                                         sizeof(lead) - 1u);
        if (found == NULL) {
            break;
        }
        cursor = (size_t)(found - text) + sizeof(lead) - 1u;
        /* State constants are exactly ql_lp_<l|r>_<pair>_<phi>; every other
           declaration under this lead carries a letter after the side. */
        if (cursor + 2u < size && (text[cursor] == 'l' || text[cursor] == 'r')
            && text[cursor + 1u] == '_' && text[cursor + 2u] >= '0' &&
            text[cursor + 2u] <= '9') {
            chcpdr_variable variable;
            uint64_t pair = 0u;
            uint64_t phi = 0u;
            size_t digits = 0u;
            memset(&variable, 0, sizeof(variable));
            variable.is_right = text[cursor] == 'r';
            cursor += 2u;
            while (cursor < size && text[cursor] >= '0' &&
                   text[cursor] <= '9') {
                pair = pair * 10u + (uint64_t)(text[cursor] - '0');
                ++cursor;
                ++digits;
            }
            if (digits == 0u || cursor >= size || text[cursor] != '_') {
                continue;
            }
            ++cursor;
            digits = 0u;
            while (cursor < size && text[cursor] >= '0' &&
                   text[cursor] <= '9') {
                phi = phi * 10u + (uint64_t)(text[cursor] - '0');
                ++cursor;
                ++digits;
            }
            if (digits == 0u) {
                continue;
            }
            if (pair != 0u) {
                ql_error_set(error, QL_STATUS_TYPE_MISMATCH,
                             "the serialized prefix carries more than one "
                             "loop pair, which this PDR does not model");
                return QL_STATUS_TYPE_MISMATCH;
            }
            variable.phi = (uint32_t)phi;
            /* " () Bool)" or " () (_ BitVec N))" follows. */
            {
                static const char bool_sort[] = " () Bool)";
                static const char bv_lead[] = " () (_ BitVec ";
                if (cursor + sizeof(bool_sort) - 1u <= size &&
                    memcmp(text + cursor, bool_sort,
                           sizeof(bool_sort) - 1u) == 0) {
                    variable.is_bool = 1u;
                    variable.width = 1u;
                } else if (cursor + sizeof(bv_lead) - 1u <= size &&
                           memcmp(text + cursor, bv_lead,
                                  sizeof(bv_lead) - 1u) == 0) {
                    uint64_t width = 0u;
                    size_t at = cursor + sizeof(bv_lead) - 1u;
                    while (at < size && text[at] >= '0' && text[at] <= '9') {
                        width = width * 10u + (uint64_t)(text[at] - '0');
                        ++at;
                    }
                    if (width == 0u || width > 64u) {
                        ql_error_set(error, QL_STATUS_TYPE_MISMATCH,
                                     "a state constant is outside the <=64 "
                                     "bit scalar fragment");
                        return QL_STATUS_TYPE_MISMATCH;
                    }
                    variable.width = (uint32_t)width;
                } else {
                    ql_error_set(error, QL_STATUS_TYPE_MISMATCH,
                                 "a state constant carries a sort this PDR "
                                 "does not model");
                    return QL_STATUS_TYPE_MISMATCH;
                }
            }
            if (*count >= capacity) {
                ql_error_set(error, QL_STATUS_TYPE_MISMATCH,
                             "the loop pair carries more state than this PDR "
                             "models");
                return QL_STATUS_TYPE_MISMATCH;
            }
            variables[(*count)++] = variable;
        }
    }
    return QL_STATUS_OK;
}

/* Requires the raw definitions PDR reads: guards, exits, and, for each state
   constant, the matching next and entry definitions. Their absence means the
   prefix took the shared/self encoding, whose transition is uninterpreted. */
static ql_status require_definitions(const ql_artifact *prefix,
                                     const chcpdr_variable *variables,
                                     size_t count, ql_error *error) {
    ql_artifact_view view;
    const char *needles[2] = {"(define-fun ql_lp_l_guard_0 ",
                              "(define-fun ql_lp_r_guard_0 "};
    size_t index;
    ql_status status;

    memset(&view, 0, sizeof(view));
    view.struct_size = sizeof(view);
    status = ql_artifact_get_view(prefix, &view, error);
    if (status != QL_STATUS_OK) {
        return status;
    }
    for (index = 0u; index < 2u; ++index) {
        if (find_bytes(view.data, view.size, needles[index],
                       strlen(needles[index])) == NULL) {
            ql_error_set(error, QL_STATUS_TYPE_MISMATCH,
                         "the serialized prefix does not define the actual "
                         "guard expressions this PDR transitions over");
            return QL_STATUS_TYPE_MISMATCH;
        }
    }
    for (index = 0u; index < count; ++index) {
        char needle[96];
        char symbol[64];
        variable_symbol(&variables[index], 1u, symbol, sizeof(symbol));
        (void)snprintf(needle, sizeof(needle), "(define-fun %s ", symbol);
        if (find_bytes(view.data, view.size, needle, strlen(needle)) ==
            NULL) {
            ql_error_set(error, QL_STATUS_TYPE_MISMATCH,
                         "the serialized prefix does not define an actual "
                         "next-state expression for every state constant");
            return QL_STATUS_TYPE_MISMATCH;
        }
        variable_symbol(&variables[index], 2u, symbol, sizeof(symbol));
        (void)snprintf(needle, sizeof(needle), "(define-fun %s ", symbol);
        if (find_bytes(view.data, view.size, needle, strlen(needle)) ==
            NULL) {
            ql_error_set(error, QL_STATUS_TYPE_MISMATCH,
                         "the serialized prefix does not define an actual "
                         "entry expression for every state constant");
            return QL_STATUS_TYPE_MISMATCH;
        }
    }
    return QL_STATUS_OK;
}

/* --- Lemmas --------------------------------------------------------------- */

typedef struct chcpdr_literal {
    uint32_t variable;
    uint64_t value;
} chcpdr_literal;

typedef struct chcpdr_lemma {
    /* Zero: the negation of a concrete cube. One: a printed relational
       formula (candidate relation or entry-anchored template). */
    uint32_t is_text;
    uint32_t init_checked;
    uint32_t init_holds;
    uint32_t reserved;
    size_t literal_count;
    chcpdr_literal literals[QL_CHCPDR_MAX_CUBE_LITERALS];
    char *current_text;
    char *next_text;
} chcpdr_lemma;

typedef struct chcpdr_state {
    const ql_allocator *allocator;
    const chcpdr_instance *instance;
    const ql_run_context_v1 *context;
    const ql_loop_proof_query *query;
    ql_loop_proof_query_view_v1 view;
    chcpdr_variable variables[QL_CHCPDR_MAX_STATE_VARIABLES];
    size_t variable_count;
    /* Precomputed formula fragments. */
    chcpdr_buffer init_formula;
    chcpdr_buffer bad_formula;
    chcpdr_buffer both_guards;
    /* Lemma pool and frame membership: member[lemma * frame_capacity + k]. */
    chcpdr_lemma *lemmas;
    size_t lemma_count;
    uint8_t *member;
    size_t frame_capacity;
    uint32_t frame_count;
    uint64_t queries_used;
    chcpdr_decision *decision;
} chcpdr_state;

static ql_status print_bv_literal(chcpdr_buffer *buffer, uint64_t value,
                                  uint32_t width, ql_error *error) {
    char text[80];
    uint32_t bit;
    size_t at = 0u;

    text[at++] = '#';
    text[at++] = 'b';
    for (bit = 0u; bit < width; ++bit) {
        text[at++] = ((value >> (width - 1u - bit)) & 1u) ? '1' : '0';
    }
    text[at] = '\0';
    return buffer_add_n(buffer, text, at, error);
}

static ql_status print_cube(const chcpdr_state *state,
                            const chcpdr_lemma *lemma, uint32_t next,
                            chcpdr_buffer *buffer, ql_error *error) {
    size_t index;
    ql_status status = buffer_add(buffer, "(and true", error);

    for (index = 0u; index < lemma->literal_count && status == QL_STATUS_OK;
         ++index) {
        const chcpdr_variable *variable =
            &state->variables[lemma->literals[index].variable];
        char symbol[64];
        variable_symbol(variable, next != 0u ? 1u : 0u, symbol,
                        sizeof(symbol));
        status = buffer_addf(buffer, error, " (= %s ", symbol);
        if (status != QL_STATUS_OK) {
            break;
        }
        if (variable->is_bool) {
            status = buffer_add(buffer,
                                lemma->literals[index].value != 0u ? "true"
                                                                   : "false",
                                error);
        } else {
            status = print_bv_literal(buffer, lemma->literals[index].value,
                                      variable->width, error);
        }
        if (status == QL_STATUS_OK) {
            status = buffer_add(buffer, ")", error);
        }
    }
    if (status == QL_STATUS_OK) {
        status = buffer_add(buffer, ")", error);
    }
    return status;
}

/* The lemma as a formula over the current or the next state family. */
static ql_status print_lemma(const chcpdr_state *state,
                             const chcpdr_lemma *lemma, uint32_t next,
                             chcpdr_buffer *buffer, ql_error *error) {
    if (lemma->is_text) {
        return buffer_add(buffer,
                          next != 0u ? lemma->next_text : lemma->current_text,
                          error);
    }
    {
        ql_status status = buffer_add(buffer, "(not ", error);
        if (status == QL_STATUS_OK) {
            status = print_cube(state, lemma, next, buffer, error);
        }
        if (status == QL_STATUS_OK) {
            status = buffer_add(buffer, ")", error);
        }
        return status;
    }
}

static ql_status assert_frame(chcpdr_state *state, uint32_t frame,
                              chcpdr_buffer *terminal, ql_error *error) {
    size_t index;
    ql_status status = QL_STATUS_OK;

    if (frame == 0u) {
        status = buffer_add(terminal, "(assert ", error);
        if (status == QL_STATUS_OK) {
            status = buffer_add(terminal, state->init_formula.data, error);
        }
        if (status == QL_STATUS_OK) {
            status = buffer_add(terminal, ")\n", error);
        }
        return status;
    }
    for (index = 0u; index < state->lemma_count && status == QL_STATUS_OK;
         ++index) {
        if (!state->member[index * state->frame_capacity + frame]) {
            continue;
        }
        status = buffer_add(terminal, "(assert ", error);
        if (status == QL_STATUS_OK) {
            status = print_lemma(state, &state->lemmas[index], 0u, terminal,
                                 error);
        }
        if (status == QL_STATUS_OK) {
            status = buffer_add(terminal, ")\n", error);
        }
    }
    return status;
}

/* --- Solver --------------------------------------------------------------- */

static uint64_t solver_deadline_ms(uint64_t method_timeout_ms,
                                   const ql_run_context_v1 *context) {
    uint64_t remaining_ns;
    uint64_t remaining_ms;

    if (context == NULL ||
        context->struct_size < offsetof(ql_run_context_v1, reserved) ||
        context->remaining_ns == NULL) {
        return method_timeout_ms;
    }
    remaining_ns = context->remaining_ns(context->cancel_state);
    if (remaining_ns == UINT64_MAX) {
        return method_timeout_ms;
    }
    remaining_ms = remaining_ns == 0u
                       ? 1u
                       : (remaining_ns + UINT64_C(999999)) / UINT64_C(1000000);
    if (method_timeout_ms == 0u || remaining_ms < method_timeout_ms) {
        return remaining_ms;
    }
    return method_timeout_ms;
}

/* One clause query: the serialized prefix plus this call's own asserts. A
   query beyond the recorded budget, or a cancelled run, is reported through
   `exhausted` so the caller leaves UNKNOWN instead of looping forever. */
static ql_status solve_terminal(chcpdr_state *state, const char *terminal,
                                size_t terminal_size, uint32_t want_model,
                                ql_solver_check_result_v1 *result,
                                uint32_t *exhausted, ql_error *error) {
    const ql_solver_descriptor_v1 *descriptor =
        ql_bitwuzla_solver_descriptor();
    const ql_run_context_v1 *context = state->context;
    ql_solver_check_request_v1 request;
    ql_artifact *artifact = NULL;
    ql_solver *solver = NULL;
    ql_status status;

    *exhausted = 0u;
    if (state->queries_used >= state->instance->max_queries) {
        *exhausted = 1u;
        return QL_STATUS_OK;
    }
    if (context != NULL && context->is_cancelled != NULL &&
        context->is_cancelled(context->cancel_state) != 0u) {
        *exhausted = 1u;
        return QL_STATUS_OK;
    }
    ++state->queries_used;
    if (descriptor->capability.availability == QL_SOLVER_UNAVAILABLE) {
        ql_error_set(error, QL_STATUS_NOT_FOUND,
                     "the canonical Bitwuzla backend is not available in this build");
        return QL_STATUS_NOT_FOUND;
    }
    status = ql_artifact_create(state->allocator, QL_ARTIFACT_KIND_SMTLIB2,
                                QL_SMTLIB2_SCHEMA_VERSION, terminal,
                                terminal_size, &artifact, error);
    if (status != QL_STATUS_OK) {
        return status;
    }
    if (context != NULL &&
        context->struct_size >= offsetof(ql_run_context_v1, reserved) &&
        context->solver_session != NULL) {
        status = ql_solver_create_in_session(
            state->allocator, (ql_solver_session *)context->solver_session,
            &solver, error);
    } else {
        status = ql_solver_create(state->allocator, descriptor,
                                  state->instance->solver_options, &solver,
                                  error);
    }
    if (status == QL_STATUS_OK) {
        status = ql_solver_add_smt2(
            solver, ql_loop_proof_query_prefix_artifact(state->query), error);
    }
    if (status == QL_STATUS_OK) {
        status = ql_solver_add_smt2(solver, artifact, error);
    }
    if (status == QL_STATUS_OK) {
        ql_solver_check_request_init(&request, state->view.logic);
        request.maximum_bv_width = state->view.maximum_bv_width;
        request.timeout_ms =
            solver_deadline_ms(state->instance->timeout_ms, context);
        if (want_model != 0u) {
            request.artifact_requests = QL_SOLVER_REQUEST_MODEL;
        }
        if (context != NULL && context->is_cancelled != NULL) {
            request.cancel_state = context->cancel_state;
            request.is_cancelled = context->is_cancelled;
        }
        ql_solver_check_result_init(result);
        status = ql_solver_check(solver, &request, result, error);
    }
    ql_solver_destroy(solver);
    ql_artifact_release(artifact);
    if (status == QL_STATUS_OK &&
        state->decision->backend_name == NULL) {
        state->decision->backend_name = result->backend_name;
        state->decision->backend_version = result->backend_version;
        state->decision->backend_binary_digest =
            result->backend_binary_digest;
    }
    return status;
}

/* --- Model reading -------------------------------------------------------- */

static int token_matches(const char *text, size_t size, const char *token) {
    size_t length = strlen(token);
    return size == length && memcmp(text, token, length) == 0;
}

static int decode_value_token(const char *text, size_t size,
                              const chcpdr_variable *variable,
                              uint64_t *value) {
    *value = 0u;
    if (variable->is_bool) {
        if (token_matches(text, size, "true")) {
            *value = 1u;
            return 1;
        }
        if (token_matches(text, size, "false")) {
            return 1;
        }
        if (size == 3u && text[0] == '#' && text[1] == 'b') {
            *value = text[2] == '1' ? 1u : 0u;
            return text[2] == '0' || text[2] == '1';
        }
        return 0;
    }
    if (size > 2u && text[0] == '#' && text[1] == 'b') {
        size_t index;
        if (size - 2u != variable->width) {
            return 0;
        }
        for (index = 2u; index < size; ++index) {
            if (text[index] != '0' && text[index] != '1') {
                return 0;
            }
            *value = (*value << 1u) | (uint64_t)(text[index] - '0');
        }
        return 1;
    }
    if (size > 2u && text[0] == '#' && text[1] == 'x') {
        size_t index;
        if ((size - 2u) * 4u != variable->width &&
            (size - 2u) * 4u < variable->width) {
            return 0;
        }
        for (index = 2u; index < size; ++index) {
            uint32_t digit;
            const char ch = text[index];
            if (ch >= '0' && ch <= '9') {
                digit = (uint32_t)(ch - '0');
            } else if (ch >= 'a' && ch <= 'f') {
                digit = (uint32_t)(ch - 'a' + 10);
            } else if (ch >= 'A' && ch <= 'F') {
                digit = (uint32_t)(ch - 'A' + 10);
            } else {
                return 0;
            }
            *value = (*value << 4u) | digit;
        }
        if (variable->width < 64u) {
            *value &= (UINT64_C(1) << variable->width) - 1u;
        }
        return 1;
    }
    if (size > 4u && text[0] == '(') {
        /* (_ bvDECIMAL WIDTH) */
        size_t cursor = 1u;
        uint64_t parsed = 0u;
        while (cursor < size &&
               (text[cursor] == ' ' || text[cursor] == '_')) {
            ++cursor;
        }
        if (cursor + 1u >= size || text[cursor] != 'b' ||
            text[cursor + 1u] != 'v') {
            return 0;
        }
        cursor += 2u;
        if (cursor >= size || text[cursor] < '0' || text[cursor] > '9') {
            return 0;
        }
        while (cursor < size && text[cursor] >= '0' && text[cursor] <= '9') {
            parsed = parsed * 10u + (uint64_t)(text[cursor] - '0');
            ++cursor;
        }
        *value = parsed;
        if (variable->width < 64u) {
            *value &= (UINT64_C(1) << variable->width) - 1u;
        }
        return 1;
    }
    return 0;
}

/* Pulls one state assignment out of a solver model. Every state constant
   must appear; a model that omits one cannot describe a predecessor and is
   treated as a decode failure. */
static ql_status read_model_cube(const chcpdr_state *state,
                                 const ql_artifact *model,
                                 chcpdr_lemma *cube, ql_error *error) {
    ql_artifact_view view;
    const char *text;
    size_t size;
    size_t variable_index;
    ql_status status;

    memset(cube, 0, sizeof(*cube));
    memset(&view, 0, sizeof(view));
    view.struct_size = sizeof(view);
    status = ql_artifact_get_view(model, &view, error);
    if (status != QL_STATUS_OK) {
        return status;
    }
    text = (const char *)view.data;
    size = view.size;
    for (variable_index = 0u; variable_index < state->variable_count;
         ++variable_index) {
        char symbol[64];
        char needle[80];
        const char *found;
        size_t cursor;
        size_t value_start = 0u;
        size_t value_size = 0u;
        uint64_t depth = 0u;
        uint64_t value = 0u;

        variable_symbol(&state->variables[variable_index], 0u, symbol,
                        sizeof(symbol));
        (void)snprintf(needle, sizeof(needle), "define-fun %s ", symbol);
        found = (const char *)find_bytes(text, size, needle,
                                         strlen(needle));
        if (found == NULL) {
            ql_error_set(error, QL_STATUS_PARSE_ERROR,
                         "the solver model omits the state constant '%s'",
                         symbol);
            return QL_STATUS_PARSE_ERROR;
        }
        /* Everything from here to the define-fun's closing paren; the value
           is the final element at depth zero relative to this point. */
        cursor = (size_t)(found - text) + strlen(needle);
        while (cursor < size) {
            const char ch = text[cursor];
            if (ch == '(') {
                if (depth == 0u) {
                    value_start = cursor;
                }
                ++depth;
            } else if (ch == ')') {
                if (depth == 0u) {
                    break;
                }
                --depth;
                if (depth == 0u) {
                    value_size = cursor + 1u - value_start;
                }
            } else if (depth == 0u && ch != ' ' && ch != '\n' && ch != '\t') {
                value_start = cursor;
                while (cursor + 1u < size && text[cursor + 1u] != ' ' &&
                       text[cursor + 1u] != ')' &&
                       text[cursor + 1u] != '\n' &&
                       text[cursor + 1u] != '\t') {
                    ++cursor;
                }
                value_size = cursor + 1u - value_start;
            }
            ++cursor;
        }
        if (value_size == 0u ||
            !decode_value_token(text + value_start, value_size,
                                &state->variables[variable_index], &value)) {
            ql_error_set(error, QL_STATUS_PARSE_ERROR,
                         "the solver model value for '%s' cannot be decoded",
                         symbol);
            return QL_STATUS_PARSE_ERROR;
        }
        cube->literals[cube->literal_count].variable =
            (uint32_t)variable_index;
        cube->literals[cube->literal_count].value = value;
        ++cube->literal_count;
    }
    return QL_STATUS_OK;
}

/* --- Query shapes --------------------------------------------------------- */

typedef enum chcpdr_answer {
    QL_CHCPDR_ANSWER_UNSAT = 0,
    QL_CHCPDR_ANSWER_SAT,
    QL_CHCPDR_ANSWER_STOPPED
} chcpdr_answer;

/* Runs one query built from `terminal`, classifying non-answers as STOPPED
   so every caller handles solver UNKNOWN, budget exhaustion, and
   cancellation the same way. */
static ql_status run_query(chcpdr_state *state, const chcpdr_buffer *terminal,
                           uint32_t want_model, chcpdr_answer *answer,
                           ql_artifact **model, ql_error *error) {
    ql_solver_check_result_v1 result;
    uint32_t exhausted = 0u;
    ql_status status;

    *answer = QL_CHCPDR_ANSWER_STOPPED;
    if (model != NULL) {
        *model = NULL;
    }
    ql_solver_check_result_init(&result);
    status = solve_terminal(state, terminal->data, terminal->size, want_model,
                            &result, &exhausted, error);
    if (status != QL_STATUS_OK) {
        return status;
    }
    if (exhausted != 0u) {
        return QL_STATUS_OK;
    }
    if (result.kind == QL_SOLVER_CHECK_UNSAT) {
        *answer = QL_CHCPDR_ANSWER_UNSAT;
    } else if (result.kind == QL_SOLVER_CHECK_SAT) {
        *answer = QL_CHCPDR_ANSWER_SAT;
        if (model != NULL && result.model_artifact != NULL) {
            ql_artifact_retain(result.model_artifact);
            *model = result.model_artifact;
        }
    }
    ql_solver_check_result_clear(&result);
    return QL_STATUS_OK;
}

/* SAT(F_frame and Bad)? */
static ql_status query_bad(chcpdr_state *state, uint32_t frame,
                           chcpdr_answer *answer, ql_artifact **model,
                           ql_error *error) {
    chcpdr_buffer terminal;
    ql_status status;

    buffer_init(&terminal, state->allocator);
    status = assert_frame(state, frame, &terminal, error);
    if (status == QL_STATUS_OK) {
        status = buffer_add(&terminal, "(assert ", error);
    }
    if (status == QL_STATUS_OK) {
        status = buffer_add(&terminal, state->bad_formula.data, error);
    }
    if (status == QL_STATUS_OK) {
        status = buffer_add(&terminal, ")\n", error);
    }
    if (status == QL_STATUS_OK) {
        status = run_query(state, &terminal, 1u, answer, model, error);
    }
    buffer_dispose(&terminal);
    return status;
}

/* SAT(Init and cube)? A cube that intersects the initial states cannot be
   blocked and cannot be generalized past that boundary. */
static ql_status query_cube_in_init(chcpdr_state *state,
                                    const chcpdr_lemma *cube,
                                    chcpdr_answer *answer, ql_error *error) {
    chcpdr_buffer terminal;
    ql_status status;

    buffer_init(&terminal, state->allocator);
    status = buffer_add(&terminal, "(assert ", error);
    if (status == QL_STATUS_OK) {
        status = buffer_add(&terminal, state->init_formula.data, error);
    }
    if (status == QL_STATUS_OK) {
        status = buffer_add(&terminal, ")\n(assert ", error);
    }
    if (status == QL_STATUS_OK) {
        status = print_cube(state, cube, 0u, &terminal, error);
    }
    if (status == QL_STATUS_OK) {
        status = buffer_add(&terminal, ")\n", error);
    }
    if (status == QL_STATUS_OK) {
        status = run_query(state, &terminal, 0u, answer, NULL, error);
    }
    buffer_dispose(&terminal);
    return status;
}

/* SAT(F_{frame-1} and not cube and both guards and cube')? UNSAT means the
   cube is unreachable from frame-1 in one step, so its negation joins the
   frame; SAT hands back the predecessor to block first. */
static ql_status query_relative(chcpdr_state *state, uint32_t frame,
                                const chcpdr_lemma *cube,
                                chcpdr_answer *answer, ql_artifact **model,
                                ql_error *error) {
    chcpdr_buffer terminal;
    ql_status status;

    buffer_init(&terminal, state->allocator);
    status = assert_frame(state, frame - 1u, &terminal, error);
    if (status == QL_STATUS_OK) {
        status = buffer_add(&terminal, "(assert (not ", error);
    }
    if (status == QL_STATUS_OK) {
        status = print_cube(state, cube, 0u, &terminal, error);
    }
    if (status == QL_STATUS_OK) {
        status = buffer_add(&terminal, "))\n(assert ", error);
    }
    if (status == QL_STATUS_OK) {
        status = buffer_add(&terminal, state->both_guards.data, error);
    }
    if (status == QL_STATUS_OK) {
        status = buffer_add(&terminal, ")\n(assert ", error);
    }
    if (status == QL_STATUS_OK) {
        status = print_cube(state, cube, 1u, &terminal, error);
    }
    if (status == QL_STATUS_OK) {
        status = buffer_add(&terminal, ")\n", error);
    }
    if (status == QL_STATUS_OK) {
        status = run_query(state, &terminal, 1u, answer, model, error);
    }
    buffer_dispose(&terminal);
    return status;
}

/* SAT(F_frame and both guards and not lemma')? UNSAT admits or pushes the
   lemma into frame+1. */
static ql_status query_lemma_step(chcpdr_state *state, uint32_t frame,
                                  const chcpdr_lemma *lemma,
                                  chcpdr_answer *answer, ql_error *error) {
    chcpdr_buffer terminal;
    ql_status status;

    buffer_init(&terminal, state->allocator);
    status = assert_frame(state, frame, &terminal, error);
    if (status == QL_STATUS_OK) {
        status = buffer_add(&terminal, "(assert ", error);
    }
    if (status == QL_STATUS_OK) {
        status = buffer_add(&terminal, state->both_guards.data, error);
    }
    if (status == QL_STATUS_OK) {
        status = buffer_add(&terminal, ")\n(assert (not ", error);
    }
    if (status == QL_STATUS_OK) {
        status = print_lemma(state, lemma, 1u, &terminal, error);
    }
    if (status == QL_STATUS_OK) {
        status = buffer_add(&terminal, "))\n", error);
    }
    if (status == QL_STATUS_OK) {
        status = run_query(state, &terminal, 0u, answer, NULL, error);
    }
    buffer_dispose(&terminal);
    return status;
}

/* SAT(Init and not lemma)? A lemma must hold at entry before any frame may
   carry it. */
static ql_status query_lemma_init(chcpdr_state *state, chcpdr_lemma *lemma,
                                  chcpdr_answer *answer, ql_error *error) {
    chcpdr_buffer terminal;
    ql_status status;

    if (lemma->init_checked) {
        *answer = lemma->init_holds ? QL_CHCPDR_ANSWER_UNSAT
                                    : QL_CHCPDR_ANSWER_SAT;
        return QL_STATUS_OK;
    }
    buffer_init(&terminal, state->allocator);
    status = buffer_add(&terminal, "(assert ", error);
    if (status == QL_STATUS_OK) {
        status = buffer_add(&terminal, state->init_formula.data, error);
    }
    if (status == QL_STATUS_OK) {
        status = buffer_add(&terminal, ")\n(assert (not ", error);
    }
    if (status == QL_STATUS_OK) {
        status = print_lemma(state, lemma, 0u, &terminal, error);
    }
    if (status == QL_STATUS_OK) {
        status = buffer_add(&terminal, "))\n", error);
    }
    if (status == QL_STATUS_OK) {
        status = run_query(state, &terminal, 0u, answer, NULL, error);
    }
    buffer_dispose(&terminal);
    if (status == QL_STATUS_OK && *answer != QL_CHCPDR_ANSWER_STOPPED) {
        lemma->init_checked = 1u;
        lemma->init_holds = *answer == QL_CHCPDR_ANSWER_UNSAT;
    }
    return status;
}

/* --- Lemma pool ----------------------------------------------------------- */

static ql_status add_lemma_to_frames(chcpdr_state *state, size_t lemma_index,
                                     uint32_t up_to_frame) {
    uint32_t frame;
    for (frame = 1u; frame <= up_to_frame; ++frame) {
        state->member[lemma_index * state->frame_capacity + frame] = 1u;
    }
    return QL_STATUS_OK;
}

static ql_status pool_add_cube(chcpdr_state *state, const chcpdr_lemma *cube,
                               size_t *index, uint32_t *full,
                               ql_error *error) {
    (void)error;
    *full = 0u;
    if (state->lemma_count >= state->instance->max_lemmas) {
        *full = 1u;
        return QL_STATUS_OK;
    }
    state->lemmas[state->lemma_count] = *cube;
    state->lemmas[state->lemma_count].is_text = 0u;
    state->lemmas[state->lemma_count].current_text = NULL;
    state->lemmas[state->lemma_count].next_text = NULL;
    *index = state->lemma_count++;
    return QL_STATUS_OK;
}

static ql_status pool_add_text(chcpdr_state *state, const char *current,
                               const char *next, size_t *index,
                               uint32_t *full, ql_error *error) {
    char *current_copy;
    char *next_copy;
    size_t current_size = strlen(current);
    size_t next_size = strlen(next);

    *full = 0u;
    if (state->lemma_count >= state->instance->max_lemmas) {
        *full = 1u;
        return QL_STATUS_OK;
    }
    current_copy = state->allocator->allocate(state->allocator->user_data,
                                              current_size + 1u);
    next_copy = state->allocator->allocate(state->allocator->user_data,
                                           next_size + 1u);
    if (current_copy == NULL || next_copy == NULL) {
        state->allocator->deallocate(state->allocator->user_data,
                                     current_copy);
        state->allocator->deallocate(state->allocator->user_data, next_copy);
        ql_error_set(error, QL_STATUS_OUT_OF_MEMORY, NULL);
        return QL_STATUS_OUT_OF_MEMORY;
    }
    memcpy(current_copy, current, current_size + 1u);
    memcpy(next_copy, next, next_size + 1u);
    memset(&state->lemmas[state->lemma_count], 0,
           sizeof(state->lemmas[0]));
    state->lemmas[state->lemma_count].is_text = 1u;
    state->lemmas[state->lemma_count].current_text = current_copy;
    state->lemmas[state->lemma_count].next_text = next_copy;
    *index = state->lemma_count++;
    return QL_STATUS_OK;
}

/* --- Seed lemmas ---------------------------------------------------------- */

static ql_status format_relation_lemma(chcpdr_state *state,
                                       const ql_loop_relation_candidate_v1 *c,
                                       uint32_t next, chcpdr_buffer *buffer,
                                       ql_error *error) {
    (void)state;
    const char *l_kind = next != 0u ? "l_next" : "l";
    const char *r_kind = next != 0u ? "r_next" : "r";
    ql_status status = buffer_addf(buffer, error, "(= ql_lp_%s_0_%zu ",
                                   l_kind, c->left_phi);

    if (status != QL_STATUS_OK) {
        return status;
    }
    if (c->kind == QL_LOOP_RELATION_EQUALITY) {
        status = buffer_addf(buffer, error, "ql_lp_%s_0_%zu)", r_kind,
                             c->right_phi);
        return status;
    }
    if (c->kind == QL_LOOP_RELATION_CONSTANT_OFFSET) {
        status = buffer_addf(buffer, error, "(bvadd ql_lp_%s_0_%zu ", r_kind,
                             c->right_phi);
        if (status == QL_STATUS_OK) {
            status = print_bv_literal(buffer, c->offset, c->bit_width, error);
        }
        if (status == QL_STATUS_OK) {
            status = buffer_add(buffer, "))", error);
        }
        return status;
    }
    status = buffer_add(buffer, "(bvadd (bvmul ", error);
    if (status == QL_STATUS_OK) {
        status = print_bv_literal(buffer, c->multiplier, c->bit_width, error);
    }
    if (status == QL_STATUS_OK) {
        status = buffer_addf(buffer, error, " ql_lp_%s_0_%zu) ", r_kind,
                             c->right_phi);
    }
    if (status == QL_STATUS_OK) {
        status = print_bv_literal(buffer, c->offset, c->bit_width, error);
    }
    if (status == QL_STATUS_OK) {
        status = buffer_add(buffer, "))", error);
    }
    return status;
}

/* The seed vocabulary: the analyzer's relation candidates, and the
   entry-anchored sum and difference of every same-width bit-vector pair.
   Every seed still passes the ordinary initiation and consecution queries
   before a frame carries it, so an unsound heuristic here cannot become an
   unsound lemma. */
static ql_status build_seed_lemmas(chcpdr_state *state, size_t *first_seed,
                                   size_t *seed_count, ql_error *error) {
    chcpdr_buffer current;
    chcpdr_buffer next;
    size_t candidate_index;
    size_t left_index;
    ql_status status = QL_STATUS_OK;

    *first_seed = state->lemma_count;
    *seed_count = 0u;
    for (candidate_index = 0u;
         candidate_index < state->view.candidate_count &&
         status == QL_STATUS_OK;
         ++candidate_index) {
        ql_loop_relation_candidate_v1 candidate;
        size_t index;
        memset(&candidate, 0, sizeof(candidate));
        candidate.struct_size = sizeof(candidate);
        status = ql_loop_proof_query_candidate_at(state->query,
                                                  candidate_index,
                                                  &candidate, error);
        if (status != QL_STATUS_OK) {
            return status;
        }
        if (candidate.bit_width == 0u || candidate.bit_width > 64u ||
            state->lemma_count >= state->instance->max_lemmas) {
            continue;
        }
        buffer_init(&current, state->allocator);
        buffer_init(&next, state->allocator);
        status = format_relation_lemma(state, &candidate, 0u, &current,
                                       error);
        if (status == QL_STATUS_OK) {
            status = format_relation_lemma(state, &candidate, 1u, &next,
                                           error);
        }
        if (status == QL_STATUS_OK) {
            uint32_t full = 0u;
            status = pool_add_text(state, current.data, next.data, &index,
                                   &full, error);
            if (status == QL_STATUS_OK && full != 0u) {
                buffer_dispose(&current);
                buffer_dispose(&next);
                break;
            }
            if (status == QL_STATUS_OK) {
                ++*seed_count;
            }
        }
        buffer_dispose(&current);
        buffer_dispose(&next);
    }

    /* Anchor template: a variable that never changes across the latch stays
       at its entry expression. This is what ties a loop-invariant copy of an
       input back to the input the guards mention; the admission queries
       reject it for anything the latch actually updates. */
    for (left_index = 0u;
         left_index < state->variable_count && status == QL_STATUS_OK;
         ++left_index) {
        char cur[64];
        char nxt[64];
        char ent[64];
        size_t index;
        uint32_t full = 0u;
        if (state->lemma_count >= state->instance->max_lemmas) {
            break;
        }
        variable_symbol(&state->variables[left_index], 0u, cur, sizeof(cur));
        variable_symbol(&state->variables[left_index], 1u, nxt, sizeof(nxt));
        variable_symbol(&state->variables[left_index], 2u, ent, sizeof(ent));
        buffer_init(&current, state->allocator);
        buffer_init(&next, state->allocator);
        status = buffer_addf(&current, error, "(= %s %s)", cur, ent);
        if (status == QL_STATUS_OK) {
            status = buffer_addf(&next, error, "(= %s %s)", nxt, ent);
        }
        if (status == QL_STATUS_OK) {
            status = pool_add_text(state, current.data, next.data, &index,
                                   &full, error);
            if (status == QL_STATUS_OK && full == 0u) {
                ++*seed_count;
            }
        }
        buffer_dispose(&current);
        buffer_dispose(&next);
        if (status == QL_STATUS_OK && full != 0u) {
            goto done;
        }
    }

    for (left_index = 0u;
         left_index < state->variable_count && status == QL_STATUS_OK;
         ++left_index) {
        size_t right_index;
        if (state->variables[left_index].is_right ||
            state->variables[left_index].is_bool) {
            continue;
        }
        for (right_index = 0u;
             right_index < state->variable_count && status == QL_STATUS_OK;
             ++right_index) {
            static const char *const operators[2] = {"bvsub", "bvadd"};
            size_t op;
            if (!state->variables[right_index].is_right ||
                state->variables[right_index].is_bool ||
                state->variables[right_index].width !=
                    state->variables[left_index].width) {
                continue;
            }
            for (op = 0u; op < 2u && status == QL_STATUS_OK; ++op) {
                char l_cur[64];
                char r_cur[64];
                char l_nxt[64];
                char r_nxt[64];
                char l_ent[64];
                char r_ent[64];
                size_t index;
                if (state->lemma_count >= state->instance->max_lemmas) {
                    break;
                }
                variable_symbol(&state->variables[left_index], 0u, l_cur,
                                sizeof(l_cur));
                variable_symbol(&state->variables[right_index], 0u, r_cur,
                                sizeof(r_cur));
                variable_symbol(&state->variables[left_index], 1u, l_nxt,
                                sizeof(l_nxt));
                variable_symbol(&state->variables[right_index], 1u, r_nxt,
                                sizeof(r_nxt));
                variable_symbol(&state->variables[left_index], 2u, l_ent,
                                sizeof(l_ent));
                variable_symbol(&state->variables[right_index], 2u, r_ent,
                                sizeof(r_ent));
                buffer_init(&current, state->allocator);
                buffer_init(&next, state->allocator);
                status = buffer_addf(&current, error,
                                     "(= (%s %s %s) (%s %s %s))",
                                     operators[op], l_cur, r_cur,
                                     operators[op], l_ent, r_ent);
                if (status == QL_STATUS_OK) {
                    status = buffer_addf(&next, error,
                                         "(= (%s %s %s) (%s %s %s))",
                                         operators[op], l_nxt, r_nxt,
                                         operators[op], l_ent, r_ent);
                }
                if (status == QL_STATUS_OK) {
                    uint32_t full = 0u;
                    status = pool_add_text(state, current.data, next.data,
                                           &index, &full, error);
                    if (status == QL_STATUS_OK && full != 0u) {
                        buffer_dispose(&current);
                        buffer_dispose(&next);
                        goto done;
                    }
                    if (status == QL_STATUS_OK) {
                        ++*seed_count;
                    }
                }
                buffer_dispose(&current);
                buffer_dispose(&next);
            }
        }
    }

done:
    return status;
}

/* --- PDR core ------------------------------------------------------------- */

typedef enum chcpdr_outcome_kind {
    QL_CHCPDR_RESULT_FIXPOINT = 0,
    QL_CHCPDR_RESULT_TRACE,
    QL_CHCPDR_RESULT_STOPPED
} chcpdr_outcome_kind;

typedef struct chcpdr_obligation {
    chcpdr_lemma cube;
    uint32_t frame;
} chcpdr_obligation;

static ql_status generalize_cube(chcpdr_state *state, uint32_t frame,
                                 chcpdr_lemma *cube, ql_error *error) {
    size_t index = 0u;

    while (index < cube->literal_count && cube->literal_count > 1u) {
        chcpdr_lemma trial = *cube;
        chcpdr_answer init_answer;
        chcpdr_answer step_answer;
        size_t move;
        ql_status status;

        for (move = index; move + 1u < trial.literal_count; ++move) {
            trial.literals[move] = trial.literals[move + 1u];
        }
        --trial.literal_count;
        status = query_cube_in_init(state, &trial, &init_answer, error);
        if (status != QL_STATUS_OK) {
            return status;
        }
        if (init_answer != QL_CHCPDR_ANSWER_UNSAT) {
            ++index;
            continue;
        }
        status = query_relative(state, frame, &trial, &step_answer, NULL,
                                error);
        if (status != QL_STATUS_OK) {
            return status;
        }
        if (step_answer != QL_CHCPDR_ANSWER_UNSAT) {
            ++index;
            continue;
        }
        *cube = trial;
        /* The literal that replaced the dropped one is examined next, so
           the cursor stays. */
    }
    return QL_STATUS_OK;
}

static ql_status block_obligations(chcpdr_state *state, chcpdr_lemma *bad,
                                   uint32_t frame,
                                   chcpdr_outcome_kind *outcome,
                                   ql_error *error) {
    chcpdr_obligation *stack;
    size_t depth = 0u;
    const size_t stack_capacity = (size_t)state->instance->max_frames + 2u;
    ql_status status = QL_STATUS_OK;

    stack = state->allocator->allocate(state->allocator->user_data,
                                       stack_capacity * sizeof(*stack));
    if (stack == NULL) {
        ql_error_set(error, QL_STATUS_OUT_OF_MEMORY, NULL);
        return QL_STATUS_OUT_OF_MEMORY;
    }
    *outcome = QL_CHCPDR_RESULT_STOPPED;
    stack[depth].cube = *bad;
    stack[depth].frame = frame;
    ++depth;
    while (depth > 0u) {
        chcpdr_obligation *top = &stack[depth - 1u];
        chcpdr_answer answer;
        ql_artifact *model = NULL;

        if (top->frame == 0u) {
            /* The chain of predecessors reached the initial frame: the bad
               state is reachable and this is a candidate refutation. */
            *outcome = QL_CHCPDR_RESULT_TRACE;
            goto cleanup;
        }
        status = query_relative(state, top->frame, &top->cube, &answer,
                                &model, error);
        if (status != QL_STATUS_OK) {
            goto cleanup;
        }
        if (answer == QL_CHCPDR_ANSWER_STOPPED) {
            goto cleanup;
        }
        if (answer == QL_CHCPDR_ANSWER_UNSAT) {
            size_t lemma_index;
            uint32_t full = 0u;
            ql_artifact_release(model);
            status = generalize_cube(state, top->frame, &top->cube, error);
            if (status != QL_STATUS_OK) {
                goto cleanup;
            }
            status = pool_add_cube(state, &top->cube, &lemma_index, &full,
                                   error);
            if (status != QL_STATUS_OK) {
                goto cleanup;
            }
            if (full != 0u) {
                goto cleanup;
            }
            (void)add_lemma_to_frames(state, lemma_index, top->frame);
            --depth;
            continue;
        }
        /* SAT: a predecessor inside F_{frame-1} must be blocked first. */
        if (model == NULL) {
            ql_error_set(error, QL_STATUS_INTERNAL_ERROR,
                         "the backend answered sat without a model");
            status = QL_STATUS_INTERNAL_ERROR;
            goto cleanup;
        }
        if (depth >= stack_capacity) {
            ql_artifact_release(model);
            goto cleanup;
        }
        status = read_model_cube(state, model, &stack[depth].cube, error);
        ql_artifact_release(model);
        if (status != QL_STATUS_OK) {
            /* A model this reader cannot decode stops the search, honestly:
               the result is UNKNOWN, never a guessed predecessor. */
            QL_LOGW(QL_CHCPDR_CATEGORY, "predecessor model unreadable: %s",
                    error->message);
            ql_error_clear(error);
            goto cleanup;
        }
        stack[depth].frame = top->frame - 1u;
        ++depth;
    }
    *outcome = QL_CHCPDR_RESULT_FIXPOINT;

cleanup:
    state->allocator->deallocate(state->allocator->user_data, stack);
    return status;
}

static int frames_equal(const chcpdr_state *state, uint32_t a, uint32_t b) {
    size_t index;
    for (index = 0u; index < state->lemma_count; ++index) {
        if (state->member[index * state->frame_capacity + a] !=
            state->member[index * state->frame_capacity + b]) {
            return 0;
        }
    }
    return 1;
}

/* The main IC3/PDR loop. On QL_CHCPDR_RESULT_FIXPOINT, `invariant_frame`
   names the frame whose lemma set is inductive. */
static ql_status pdr_search(chcpdr_state *state, size_t first_seed,
                            size_t seed_count,
                            chcpdr_outcome_kind *outcome,
                            uint32_t *invariant_frame, ql_error *error) {
    uint32_t frontier = 1u;
    ql_status status = QL_STATUS_OK;

    *outcome = QL_CHCPDR_RESULT_STOPPED;
    *invariant_frame = 0u;
    {
        chcpdr_answer answer;
        ql_artifact *model = NULL;
        status = query_bad(state, 0u, &answer, &model, error);
        ql_artifact_release(model);
        if (status != QL_STATUS_OK) {
            return status;
        }
        if (answer == QL_CHCPDR_ANSWER_SAT) {
            *outcome = QL_CHCPDR_RESULT_TRACE;
            return QL_STATUS_OK;
        }
        if (answer == QL_CHCPDR_ANSWER_STOPPED) {
            return QL_STATUS_OK;
        }
    }

    for (;;) {
        size_t seed;
        uint32_t level;

        if (frontier >= state->instance->max_frames) {
            return QL_STATUS_OK;
        }
        state->frame_count = frontier + 1u;
        state->decision->frames_used = frontier;

        /* Seeds are re-offered at every frontier: a seed inadmissible at an
           early, tight frame becomes admissible once the frame widens. */
        for (seed = first_seed; seed < first_seed + seed_count; ++seed) {
            chcpdr_answer answer;
            if (state->member[seed * state->frame_capacity + frontier]) {
                continue;
            }
            status = query_lemma_init(state, &state->lemmas[seed], &answer,
                                      error);
            if (status != QL_STATUS_OK) {
                return status;
            }
            if (answer == QL_CHCPDR_ANSWER_STOPPED) {
                return QL_STATUS_OK;
            }
            if (answer != QL_CHCPDR_ANSWER_UNSAT) {
                continue;
            }
            status = query_lemma_step(state, frontier - 1u,
                                      &state->lemmas[seed], &answer, error);
            if (status != QL_STATUS_OK) {
                return status;
            }
            if (answer == QL_CHCPDR_ANSWER_STOPPED) {
                return QL_STATUS_OK;
            }
            if (answer == QL_CHCPDR_ANSWER_UNSAT) {
                (void)add_lemma_to_frames(state, seed, frontier);
            }
        }

        /* Strengthen the frontier until no bad state is left inside it. */
        for (;;) {
            chcpdr_answer answer;
            ql_artifact *model = NULL;
            chcpdr_lemma bad_cube;
            chcpdr_outcome_kind block_outcome;

            status = query_bad(state, frontier, &answer, &model, error);
            if (status != QL_STATUS_OK) {
                return status;
            }
            if (answer == QL_CHCPDR_ANSWER_STOPPED) {
                ql_artifact_release(model);
                return QL_STATUS_OK;
            }
            if (answer == QL_CHCPDR_ANSWER_UNSAT) {
                ql_artifact_release(model);
                break;
            }
            if (model == NULL) {
                ql_error_set(error, QL_STATUS_INTERNAL_ERROR,
                             "the backend answered sat without a model");
                return QL_STATUS_INTERNAL_ERROR;
            }
            status = read_model_cube(state, model, &bad_cube, error);
            ql_artifact_release(model);
            if (status != QL_STATUS_OK) {
                QL_LOGW(QL_CHCPDR_CATEGORY, "bad-state model unreadable: %s",
                        error->message);
                ql_error_clear(error);
                return QL_STATUS_OK;
            }
            status = block_obligations(state, &bad_cube, frontier,
                                       &block_outcome, error);
            if (status != QL_STATUS_OK) {
                return status;
            }
            if (block_outcome == QL_CHCPDR_RESULT_TRACE) {
                *outcome = QL_CHCPDR_RESULT_TRACE;
                return QL_STATUS_OK;
            }
            if (block_outcome == QL_CHCPDR_RESULT_STOPPED) {
                return QL_STATUS_OK;
            }
        }

        /* Push lemmas forward; two equal adjacent frames are the fixpoint. */
        for (level = 1u; level <= frontier; ++level) {
            size_t index;
            for (index = 0u; index < state->lemma_count; ++index) {
                chcpdr_answer answer;
                if (!state->member[index * state->frame_capacity + level] ||
                    state->member[index * state->frame_capacity + level +
                                  1u]) {
                    continue;
                }
                status = query_lemma_step(state, level,
                                          &state->lemmas[index], &answer,
                                          error);
                if (status != QL_STATUS_OK) {
                    return status;
                }
                if (answer == QL_CHCPDR_ANSWER_STOPPED) {
                    return QL_STATUS_OK;
                }
                if (answer == QL_CHCPDR_ANSWER_UNSAT) {
                    state->member[index * state->frame_capacity + level +
                                  1u] = 1u;
                }
            }
            if (frames_equal(state, level, level + 1u)) {
                *outcome = QL_CHCPDR_RESULT_FIXPOINT;
                *invariant_frame = level + 1u;
                return QL_STATUS_OK;
            }
        }
        ++frontier;
    }
}

/* Re-verifies the fixpoint with three fresh queries before anything is
   promoted: initiation, consecution, and bad-state exclusion. The PDR
   bookkeeping above is not trusted with the final claim. */
static ql_status verify_invariant(chcpdr_state *state, uint32_t frame,
                                  uint32_t *verified, ql_error *error) {
    chcpdr_buffer terminal;
    chcpdr_answer answer;
    size_t index;
    ql_status status;

    *verified = 0u;

    /* Initiation: Init implies every lemma; checked lemma by lemma so a
       failure names its lemma in the log. */
    for (index = 0u; index < state->lemma_count; ++index) {
        if (!state->member[index * state->frame_capacity + frame]) {
            continue;
        }
        status = query_lemma_init(state, &state->lemmas[index], &answer,
                                  error);
        if (status != QL_STATUS_OK) {
            return status;
        }
        if (answer != QL_CHCPDR_ANSWER_UNSAT) {
            return QL_STATUS_OK;
        }
    }
    /* Consecution: F and both guards implies F'. */
    for (index = 0u; index < state->lemma_count; ++index) {
        if (!state->member[index * state->frame_capacity + frame]) {
            continue;
        }
        status = query_lemma_step(state, frame, &state->lemmas[index],
                                  &answer, error);
        if (status != QL_STATUS_OK) {
            return status;
        }
        if (answer != QL_CHCPDR_ANSWER_UNSAT) {
            return QL_STATUS_OK;
        }
    }
    /* Exclusion: F and Bad is unsatisfiable. */
    buffer_init(&terminal, state->allocator);
    status = assert_frame(state, frame, &terminal, error);
    if (status == QL_STATUS_OK) {
        status = buffer_add(&terminal, "(assert ", error);
    }
    if (status == QL_STATUS_OK) {
        status = buffer_add(&terminal, state->bad_formula.data, error);
    }
    if (status == QL_STATUS_OK) {
        status = buffer_add(&terminal, ")\n", error);
    }
    if (status == QL_STATUS_OK) {
        status = run_query(state, &terminal, 0u, &answer, NULL, error);
    }
    buffer_dispose(&terminal);
    if (status != QL_STATUS_OK) {
        return status;
    }
    if (answer == QL_CHCPDR_ANSWER_UNSAT) {
        *verified = 1u;
    }
    return QL_STATUS_OK;
}

/* The comparison domain must be inhabited before an UNSAT-shaped claim may
   be promoted, exactly as on the SMT product paths. */
static ql_status check_domain(chcpdr_state *state, uint32_t *inhabited,
                              ql_error *error) {
    const ql_artifact *domain =
        ql_loop_proof_query_domain_artifact(state->query);
    ql_artifact_view view;
    chcpdr_buffer terminal;
    chcpdr_answer answer;
    ql_status status;

    *inhabited = 0u;
    if (domain == NULL) {
        return QL_STATUS_OK;
    }
    memset(&view, 0, sizeof(view));
    view.struct_size = sizeof(view);
    status = ql_artifact_get_view(domain, &view, error);
    if (status != QL_STATUS_OK) {
        return status;
    }
    buffer_init(&terminal, state->allocator);
    status = buffer_add_n(&terminal, (const char *)view.data, view.size,
                          error);
    if (status == QL_STATUS_OK) {
        status = run_query(state, &terminal, 0u, &answer, NULL, error);
    }
    buffer_dispose(&terminal);
    if (status != QL_STATUS_OK) {
        return status;
    }
    if (answer == QL_CHCPDR_ANSWER_SAT) {
        *inhabited = 1u;
    }
    return QL_STATUS_OK;
}

/* --- The remainder: options, capability, outcome, method entry ------------ */

static void set_diagnostic(chcpdr_decision *decision, const char *message) {
    size_t length = strlen(message);
    if (length >= sizeof(decision->diagnostic)) {
        length = sizeof(decision->diagnostic) - 1u;
    }
    memcpy(decision->diagnostic, message, length);
    decision->diagnostic[length] = '\0';
}

static void *chcpdr_json_allocate(void *context, size_t size) {
    ql_allocator *allocator = (ql_allocator *)context;
    return allocator->allocate(allocator->user_data, size);
}

static void *chcpdr_json_reallocate(void *context, void *pointer,
                                    size_t old_size, size_t size) {
    ql_allocator *allocator = (ql_allocator *)context;
    (void)old_size;
    return allocator->reallocate(allocator->user_data, pointer, size);
}

static void chcpdr_json_deallocate(void *context, void *pointer) {
    ql_allocator *allocator = (ql_allocator *)context;
    allocator->deallocate(allocator->user_data, pointer);
}

static yyjson_alc chcpdr_json_allocator(ql_allocator *allocator) {
    yyjson_alc result;
    result.malloc = chcpdr_json_allocate;
    result.realloc = chcpdr_json_reallocate;
    result.free = chcpdr_json_deallocate;
    result.ctx = allocator;
    return result;
}

static ql_status parse_options(chcpdr_instance *instance,
                               const char *options_json, ql_error *error) {
    static const char *const known[] = {"unsat_promotion", "timeout_ms",
                                        "max_frames", "max_lemmas",
                                        "max_queries", "solver_options"};
    yyjson_alc json_allocator = chcpdr_json_allocator(&instance->allocator);
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
                     "invalid %s options at byte %zu: %s",
                     QL_CHCPDR_METHOD_NAME, read_error.pos,
                     read_error.msg != NULL ? read_error.msg : "parse error");
        return QL_STATUS_PARSE_ERROR;
    }
    root = yyjson_doc_get_root(document);
    if (!yyjson_is_obj(root)) {
        ql_error_set(error, QL_STATUS_PARSE_ERROR,
                     "%s options must be a JSON object",
                     QL_CHCPDR_METHOD_NAME);
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
                         QL_CHCPDR_METHOD_NAME, name != NULL ? name : "");
            status = QL_STATUS_INVALID_ARGUMENT;
            goto cleanup;
        }
    }

    value = yyjson_obj_get(root, "unsat_promotion");
    if (value != NULL) {
        const char *text = yyjson_get_str(value);
        if (text != NULL && strcmp(text, "none") == 0) {
            instance->unsat_promotion = QL_CHCPDR_PROMOTION_NONE;
        } else if (text != NULL && strcmp(text, "trusted-backend") == 0) {
            instance->unsat_promotion = QL_CHCPDR_PROMOTION_TRUSTED_BACKEND;
        } else {
            ql_error_set(error, QL_STATUS_INVALID_ARGUMENT,
                         "unsat_promotion must be \"none\" or \"trusted-backend\"");
            status = QL_STATUS_INVALID_ARGUMENT;
            goto cleanup;
        }
    }
    value = yyjson_obj_get(root, "timeout_ms");
    if (value != NULL) {
        if (!yyjson_is_uint(value)) {
            ql_error_set(error, QL_STATUS_INVALID_ARGUMENT,
                         "timeout_ms must be a non-negative integer");
            status = QL_STATUS_INVALID_ARGUMENT;
            goto cleanup;
        }
        instance->timeout_ms = yyjson_get_uint(value);
    }
    value = yyjson_obj_get(root, "max_frames");
    if (value != NULL) {
        if (!yyjson_is_uint(value) || yyjson_get_uint(value) == 0u ||
            yyjson_get_uint(value) > 1024u) {
            ql_error_set(error, QL_STATUS_INVALID_ARGUMENT,
                         "max_frames must be between 1 and 1024");
            status = QL_STATUS_INVALID_ARGUMENT;
            goto cleanup;
        }
        instance->max_frames = yyjson_get_uint(value);
    }
    value = yyjson_obj_get(root, "max_lemmas");
    if (value != NULL) {
        if (!yyjson_is_uint(value) || yyjson_get_uint(value) == 0u ||
            yyjson_get_uint(value) > 65536u) {
            ql_error_set(error, QL_STATUS_INVALID_ARGUMENT,
                         "max_lemmas must be between 1 and 65536");
            status = QL_STATUS_INVALID_ARGUMENT;
            goto cleanup;
        }
        instance->max_lemmas = yyjson_get_uint(value);
    }
    value = yyjson_obj_get(root, "max_queries");
    if (value != NULL) {
        if (!yyjson_is_uint(value) || yyjson_get_uint(value) == 0u ||
            yyjson_get_uint(value) > 1000000u) {
            ql_error_set(error, QL_STATUS_INVALID_ARGUMENT,
                         "max_queries must be between 1 and 1000000");
            status = QL_STATUS_INVALID_ARGUMENT;
            goto cleanup;
        }
        instance->max_queries = yyjson_get_uint(value);
    }
    value = yyjson_obj_get(root, "solver_options");
    if (value != NULL) {
        const char *text;
        size_t length;
        if (!yyjson_is_str(value)) {
            ql_error_set(error, QL_STATUS_INVALID_ARGUMENT,
                         "solver_options must be a JSON string");
            status = QL_STATUS_INVALID_ARGUMENT;
            goto cleanup;
        }
        text = yyjson_get_str(value);
        length = yyjson_get_len(value);
        instance->solver_options = instance->allocator.allocate(
            instance->allocator.user_data, length + 1u);
        if (instance->solver_options == NULL) {
            ql_error_set(error, QL_STATUS_OUT_OF_MEMORY, NULL);
            status = QL_STATUS_OUT_OF_MEMORY;
            goto cleanup;
        }
        memcpy(instance->solver_options, text, length);
        instance->solver_options[length] = '\0';
    }

cleanup:
    yyjson_doc_free(document);
    return status;
}

static ql_status QL_CALL chcpdr_capability(
    const char *options_json, ql_proof_method_capability_v1 *capability,
    ql_error *error) {
    chcpdr_instance probe;
    ql_status status;

    memset(&probe, 0, sizeof(probe));
    probe.allocator = *ql_default_allocator();
    probe.timeout_ms = QL_CHCPDR_DEFAULT_TIMEOUT_MS;
    probe.max_frames = QL_CHCPDR_DEFAULT_MAX_FRAMES;
    probe.max_lemmas = QL_CHCPDR_DEFAULT_MAX_LEMMAS;
    probe.max_queries = QL_CHCPDR_DEFAULT_MAX_QUERIES;
    status = parse_options(&probe, options_json, error);
    if (probe.solver_options != NULL) {
        probe.allocator.deallocate(probe.allocator.user_data,
                                   probe.solver_options);
    }
    if (status != QL_STATUS_OK) {
        return status;
    }
    capability->family = QL_PROOF_METHOD_FAMILY_CHC_PDR;
    capability->soundness_classes = QL_PROOF_SOUNDNESS_NONE;
    capability->result_kinds = QL_PROOF_RESULT_UNKNOWN;
    if (probe.unsat_promotion == QL_CHCPDR_PROMOTION_TRUSTED_BACKEND) {
        capability->soundness_classes = QL_PROOF_SOUNDNESS_PROOF;
        capability->result_kinds |= QL_PROOF_RESULT_PROOF;
    }
    capability->supported_relations = QL_PROOF_RELATION_ALL;
    capability->supported_ub_policies = QL_PROOF_UB_ALL;
    capability->supported_observations = QL_OBSERVE_ALL;
    capability->supported_memory_observations =
        QL_PROOF_MEMORY_MODE(QL_MEMORY_IGNORE) |
        QL_PROOF_MEMORY_MODE(QL_MEMORY_FINAL_REACHABLE_STATE);
    capability->supported_external_call_observations =
        QL_PROOF_EXTERNAL_CALL_MODE(QL_EXTERNAL_CALLS_IGNORE) |
        QL_PROOF_EXTERNAL_CALL_MODE(QL_EXTERNAL_CALLS_ORDERED_TRACE);
    capability->flags = QL_PROOF_CAPABILITY_PRECONDITIONS;
    ql_error_clear(error);
    return QL_STATUS_OK;
}

/* --- Lowering ------------------------------------------------------------- */

typedef struct chcpdr_side {
    ql_c_frontend_unit *unit;
    ql_c_lower_result *result;
    ql_ir *ir;
} chcpdr_side;

static void chcpdr_side_dispose(chcpdr_side *side) {
    ql_ir_release(side->ir);
    ql_c_lower_result_destroy(side->result);
    ql_c_frontend_unit_destroy(side->unit);
    memset(side, 0, sizeof(*side));
}

static ql_status lower_side(const ql_allocator *allocator, const char *source,
                            size_t source_size, const char *name,
                            size_t name_size, chcpdr_side *side,
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

static ql_status positional_binding_match(const ql_problem *problem,
                                          const ql_problem_view_v2 *view,
                                          uint32_t *matches,
                                          ql_error *error) {
    size_t index;
    ql_status status;

    *matches = 0u;
    status = ql_problem_require_proof_binding(problem, error);
    if (status != QL_STATUS_OK) {
        ql_error_clear(error);
        return QL_STATUS_OK;
    }
    for (index = 0u; index < view->argument_binding_count; ++index) {
        ql_problem_argument_binding_v1 binding;
        memset(&binding, 0, sizeof(binding));
        binding.struct_size = sizeof(binding);
        status = ql_problem_argument_binding_at(problem, index, &binding,
                                                error);
        if (status != QL_STATUS_OK) {
            return status;
        }
        if (binding.left_index != binding.right_index) {
            ql_error_clear(error);
            return QL_STATUS_OK;
        }
    }
    *matches = 1u;
    ql_error_clear(error);
    return QL_STATUS_OK;
}

/* --- Outcome -------------------------------------------------------------- */

static int add_digest(yyjson_mut_doc *document, yyjson_mut_val *object,
                      const char *key, const ql_digest *digest) {
    char hex[QL_DIGEST_HEX_SIZE];
    ql_digest_hex(digest, hex);
    return yyjson_mut_obj_add_strcpy(document, object, key, hex);
}

static ql_status compute_cache_key(const ql_digest *problem_digest,
                                   const chcpdr_instance *instance,
                                   const ql_digest *prefix_digest,
                                   const ql_digest *backend_binary,
                                   ql_digest *cache_key, ql_error *error) {
    char identity[704];
    char prefix_hex[QL_DIGEST_HEX_SIZE];
    char backend_hex[QL_DIGEST_HEX_SIZE];
    ql_cache_key_input_v1 input;
    int written;

    ql_digest_hex(prefix_digest, prefix_hex);
    ql_digest_hex(backend_binary, backend_hex);
    written = snprintf(
        identity, sizeof(identity),
        "{\"promotion\":%u,\"timeout_ms\":%llu,\"max_frames\":%llu,"
        "\"max_lemmas\":%llu,\"max_queries\":%llu,\"backend\":\"%s\","
        "\"prefix\":\"%s\"}",
        (unsigned)instance->unsat_promotion,
        (unsigned long long)instance->timeout_ms,
        (unsigned long long)instance->max_frames,
        (unsigned long long)instance->max_lemmas,
        (unsigned long long)instance->max_queries, backend_hex, prefix_hex);
    if (written < 0 || (size_t)written >= sizeof(identity)) {
        ql_error_set(error, QL_STATUS_INTERNAL_ERROR,
                     "could not format the cache identity");
        return QL_STATUS_INTERNAL_ERROR;
    }
    memset(&input, 0, sizeof(input));
    input.struct_size = sizeof(input);
    input.artifact_digest = *problem_digest;
    input.semantic_problem_digest = *problem_digest;
    input.method_name = QL_CHCPDR_METHOD_NAME;
    input.method_version = QL_CHCPDR_METHOD_VERSION;
    input.canonical_options = identity;
    input.canonical_options_size = (size_t)written;
    return ql_cache_key_compute(&input, cache_key, error);
}

static ql_status build_outcome(const ql_allocator *allocator,
                               const chcpdr_instance *instance,
                               const ql_problem_view_v2 *problem_view,
                               const ql_digest *prefix_digest,
                               const chcpdr_decision *decision,
                               const chcpdr_state *state,
                               uint32_t invariant_frame,
                               ql_artifact **output, ql_error *error) {
    ql_allocator allocator_copy = *allocator;
    yyjson_alc json_allocator = chcpdr_json_allocator(&allocator_copy);
    yyjson_mut_doc *document = NULL;
    yyjson_mut_val *root;
    yyjson_mut_val *search_object;
    yyjson_mut_val *invariant_array;
    yyjson_mut_val *trust_object;
    yyjson_write_err write_error;
    ql_digest cache_key;
    char *json = NULL;
    size_t json_size = 0u;
    ql_status status;

    status = compute_cache_key(&problem_view->artifact_digest, instance,
                               prefix_digest,
                               &decision->backend_binary_digest, &cache_key,
                               error);
    if (status != QL_STATUS_OK) {
        return status;
    }
    document = yyjson_mut_doc_new(&json_allocator);
    root = document != NULL ? yyjson_mut_obj(document) : NULL;
    search_object = document != NULL ? yyjson_mut_obj(document) : NULL;
    invariant_array = document != NULL ? yyjson_mut_arr(document) : NULL;
    trust_object = document != NULL ? yyjson_mut_obj(document) : NULL;
    if (document == NULL || root == NULL || search_object == NULL ||
        invariant_array == NULL || trust_object == NULL ||
        !yyjson_mut_obj_add_str(document, root, "kind",
                                QL_ARTIFACT_KIND_OUTCOME) ||
        !yyjson_mut_obj_add_uint(document, root, "schema_version",
                                 QL_CHCPDR_OUTCOME_SCHEMA_VERSION) ||
        !yyjson_mut_obj_add_str(document, root, "method",
                                QL_CHCPDR_METHOD_NAME) ||
        !yyjson_mut_obj_add_str(document, root, "method_version",
                                QL_CHCPDR_METHOD_VERSION) ||
        !yyjson_mut_obj_add_str(document, root, "verdict",
                                ql_verdict_string(decision->verdict)) ||
        !yyjson_mut_obj_add_str(
            document, root, "evidence_class",
            ql_evidence_class_string(decision->evidence_class)) ||
        !add_digest(document, root, "problem_digest",
                    &problem_view->artifact_digest) ||
        !add_digest(document, root, "cache_key", &cache_key) ||
        !add_digest(document, root, "prefix_digest", prefix_digest) ||
        !yyjson_mut_obj_add_strcpy(document, root, "diagnostic",
                                   decision->diagnostic)) {
        status = QL_STATUS_OUT_OF_MEMORY;
        ql_error_set(error, status, NULL);
        goto cleanup;
    }
    if (!yyjson_mut_obj_add_uint(document, search_object, "frames_used",
                                 decision->frames_used) ||
        !yyjson_mut_obj_add_uint(document, search_object, "lemma_count",
                                 decision->lemma_count) ||
        !yyjson_mut_obj_add_uint(document, search_object, "queries_used",
                                 decision->queries_used) ||
        !yyjson_mut_obj_add_bool(document, search_object,
                                 "invariant_verified",
                                 decision->invariant_verified != 0u) ||
        !yyjson_mut_obj_add_str(document, search_object, "backend",
                                decision->backend_name != NULL
                                    ? decision->backend_name
                                    : "") ||
        !yyjson_mut_obj_add_str(document, search_object, "backend_version",
                                decision->backend_version != NULL
                                    ? decision->backend_version
                                    : "") ||
        !add_digest(document, search_object, "backend_binary_digest",
                    &decision->backend_binary_digest) ||
        !yyjson_mut_obj_add_val(document, root, "search", search_object)) {
        status = QL_STATUS_OUT_OF_MEMORY;
        ql_error_set(error, status, NULL);
        goto cleanup;
    }
    /* The invariant is recorded lemma by lemma over the prefix's own state
       symbols, so an external checker can re-discharge initiation,
       consecution, and exclusion against the same prefix. */
    if (state != NULL && invariant_frame != 0u) {
        size_t index;
        for (index = 0u; index < state->lemma_count; ++index) {
            chcpdr_buffer text;
            if (!state->member[index * state->frame_capacity +
                               invariant_frame]) {
                continue;
            }
            buffer_init(&text, allocator);
            status = print_lemma(state, &state->lemmas[index], 0u, &text,
                                 error);
            if (status == QL_STATUS_OK &&
                !yyjson_mut_arr_add_strcpy(document, invariant_array,
                                           text.data)) {
                status = QL_STATUS_OUT_OF_MEMORY;
                ql_error_set(error, status, NULL);
            }
            buffer_dispose(&text);
            if (status != QL_STATUS_OK) {
                goto cleanup;
            }
        }
    }
    if (!yyjson_mut_obj_add_val(document, root, "invariant",
                                invariant_array)) {
        status = QL_STATUS_OUT_OF_MEMORY;
        ql_error_set(error, status, NULL);
        goto cleanup;
    }
    if (!yyjson_mut_obj_add_bool(document, trust_object, "checked_proof",
                                 0) ||
        !yyjson_mut_obj_add_bool(document, trust_object, "replay_confirmed",
                                 0) ||
        !yyjson_mut_obj_add_str(
            document, trust_object, "basis",
            decision->verdict != QL_VERDICT_UNKNOWN
                ? "an inductive invariant excludes every guard-misaligned and exit-divergent state under the recorded trusted-backend policy"
                : "no sound conclusion was reached") ||
        !yyjson_mut_obj_add_val(document, root, "trust", trust_object)) {
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
                                QL_CHCPDR_OUTCOME_SCHEMA_VERSION, json,
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

static ql_status QL_CALL chcpdr_create(const ql_host_v1 *host,
                                       const char *options_json,
                                       void **instance, ql_error *error) {
    chcpdr_instance *created;
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
    created->timeout_ms = QL_CHCPDR_DEFAULT_TIMEOUT_MS;
    created->max_frames = QL_CHCPDR_DEFAULT_MAX_FRAMES;
    created->max_lemmas = QL_CHCPDR_DEFAULT_MAX_LEMMAS;
    created->max_queries = QL_CHCPDR_DEFAULT_MAX_QUERIES;
    status = parse_options(created, options_json, error);
    if (status != QL_STATUS_OK) {
        if (created->solver_options != NULL) {
            allocator.deallocate(allocator.user_data,
                                 created->solver_options);
        }
        allocator.deallocate(allocator.user_data, created);
        return status;
    }
    *instance = created;
    ql_error_clear(error);
    return QL_STATUS_OK;
}

static void QL_CALL chcpdr_destroy(void *instance) {
    chcpdr_instance *owned = (chcpdr_instance *)instance;
    ql_allocator allocator;

    if (owned == NULL) {
        return;
    }
    allocator = owned->allocator;
    if (owned->solver_options != NULL) {
        allocator.deallocate(allocator.user_data, owned->solver_options);
    }
    allocator.deallocate(allocator.user_data, owned);
}

static ql_status QL_CALL chcpdr_validate(void *instance,
                                         ql_artifact *const *inputs,
                                         size_t input_count,
                                         ql_error *error) {
    ql_artifact_view view;
    ql_status status;

    (void)instance;
    if (inputs == NULL || input_count != 1u || inputs[0] == NULL) {
        ql_error_set(error, QL_STATUS_INVALID_ARGUMENT,
                     "%s consumes exactly one quodlibet.problem artifact",
                     QL_CHCPDR_METHOD_NAME);
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
                     QL_CHCPDR_METHOD_NAME);
        return QL_STATUS_TYPE_MISMATCH;
    }
    if (view.schema_version != QL_PROBLEM_SCHEMA_VERSION_2) {
        ql_error_set(error, QL_STATUS_TYPE_MISMATCH,
                     "%s requires problem schema v2; schema v%u records no argument correspondence",
                     QL_CHCPDR_METHOD_NAME, view.schema_version);
        return QL_STATUS_TYPE_MISMATCH;
    }
    ql_error_clear(error);
    return QL_STATUS_OK;
}

static ql_verdict proved_verdict_for(ql_relation relation) {
    switch (relation) {
    case QL_RELATION_LEFT_REFINES_RIGHT:
        return QL_VERDICT_PROVED_LEFT_REFINES_RIGHT;
    case QL_RELATION_RIGHT_REFINES_LEFT:
        return QL_VERDICT_PROVED_RIGHT_REFINES_LEFT;
    default:
        return QL_VERDICT_PROVED_EQUIVALENT;
    }
}

static void state_dispose(chcpdr_state *state) {
    size_t index;
    if (state->lemmas != NULL) {
        for (index = 0u; index < state->lemma_count; ++index) {
            state->allocator->deallocate(state->allocator->user_data,
                                         state->lemmas[index].current_text);
            state->allocator->deallocate(state->allocator->user_data,
                                         state->lemmas[index].next_text);
        }
        state->allocator->deallocate(state->allocator->user_data,
                                     state->lemmas);
    }
    state->allocator->deallocate(state->allocator->user_data, state->member);
    buffer_dispose(&state->init_formula);
    buffer_dispose(&state->bad_formula);
    buffer_dispose(&state->both_guards);
}

static ql_status build_fixed_formulas(chcpdr_state *state, ql_error *error) {
    size_t index;
    ql_status status;

    buffer_init(&state->init_formula, state->allocator);
    buffer_init(&state->bad_formula, state->allocator);
    buffer_init(&state->both_guards, state->allocator);
    status = buffer_add(&state->init_formula, "(and true", error);
    for (index = 0u; index < state->variable_count && status == QL_STATUS_OK;
         ++index) {
        char current[64];
        char entry[64];
        variable_symbol(&state->variables[index], 0u, current,
                        sizeof(current));
        variable_symbol(&state->variables[index], 2u, entry, sizeof(entry));
        status = buffer_addf(&state->init_formula, error, " (= %s %s)",
                             current, entry);
    }
    if (status == QL_STATUS_OK) {
        status = buffer_add(&state->init_formula, ")", error);
    }
    if (status == QL_STATUS_OK) {
        status = buffer_add(
            &state->bad_formula,
            "(or (not (= ql_lp_l_guard_0 ql_lp_r_guard_0)) "
            "(and (not ql_lp_l_guard_0) (not ql_lp_r_guard_0) "
            "(not (= ql_lp_l_exit_0 ql_lp_r_exit_0))))",
            error);
    }
    if (status == QL_STATUS_OK) {
        status = buffer_add(&state->both_guards,
                            "(and ql_lp_l_guard_0 ql_lp_r_guard_0)", error);
    }
    return status;
}

static ql_status QL_CALL chcpdr_run(void *instance,
                                    const ql_run_context_v1 *context,
                                    ql_artifact *const *inputs,
                                    size_t input_count, ql_artifact **output,
                                    ql_error *error) {
    chcpdr_instance *owned = (chcpdr_instance *)instance;
    const ql_allocator *allocator;
    ql_problem *problem = NULL;
    ql_problem_view_v2 problem_view;
    ql_loop_proof_query *query = NULL;
    ql_loop_proof_options_v1 loop_options;
    chcpdr_side left;
    chcpdr_side right;
    chcpdr_decision decision;
    chcpdr_state state;
    ql_digest prefix_digest;
    uint32_t left_supported = 0u;
    uint32_t right_supported = 0u;
    uint32_t binding_match = 0u;
    uint32_t invariant_frame = 0u;
    uint32_t state_ready = 0u;
    ql_status status;

    memset(&left, 0, sizeof(left));
    memset(&right, 0, sizeof(right));
    memset(&decision, 0, sizeof(decision));
    memset(&state, 0, sizeof(state));
    memset(&prefix_digest, 0, sizeof(prefix_digest));
    decision.verdict = QL_VERDICT_UNKNOWN;
    decision.evidence_class = QL_EVIDENCE_UNKNOWN;
    if (owned == NULL || context == NULL || output == NULL) {
        ql_error_set(error, QL_STATUS_INVALID_ARGUMENT,
                     "instance, run context, and output are required");
        return QL_STATUS_INVALID_ARGUMENT;
    }
    *output = NULL;
    status = chcpdr_validate(instance, inputs, input_count, error);
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
    if (left_supported == 0u || right_supported == 0u) {
        set_diagnostic(&decision,
                       "the semantic C lowering does not support one of the two functions");
        status = build_outcome(allocator, owned, &problem_view,
                               &prefix_digest, &decision, NULL, 0u, output,
                               error);
        goto cleanup;
    }

    status = positional_binding_match(problem, &problem_view, &binding_match,
                                      error);
    if (status != QL_STATUS_OK) {
        goto cleanup;
    }
    ql_loop_proof_options_init(&loop_options);
    loop_options.precondition_is_true =
        problem_view.contract.precondition_json == NULL ? 1u : 0u;
    loop_options.contract_binding_match = binding_match;
    status = ql_loop_proof_query_build(allocator, left.ir, right.ir,
                                       &loop_options, &query, error);
    if (status == QL_STATUS_TYPE_MISMATCH) {
        set_diagnostic(&decision, error->message);
        status = build_outcome(allocator, owned, &problem_view,
                               &prefix_digest, &decision, NULL, 0u, output,
                               error);
        goto cleanup;
    }
    if (status != QL_STATUS_OK) {
        goto cleanup;
    }
    state.view.struct_size = sizeof(state.view);
    status = ql_loop_proof_query_get_view(query, &state.view, error);
    if (status != QL_STATUS_OK) {
        goto cleanup;
    }
    prefix_digest = state.view.prefix_digest;
    if (ql_loop_proof_query_prefix_artifact(query) == NULL ||
        (state.view.disposition != QL_LOOP_PROOF_QUERY_READY &&
         !(state.view.disposition == QL_LOOP_PROOF_CHC_PDR_UNAVAILABLE &&
           state.view.chc_pdr_available != 0u))) {
        set_diagnostic(&decision,
                       state.view.diagnostic[0] != '\0'
                           ? state.view.diagnostic
                           : "the loop pair is outside the serialized relational transition fragment");
        status = build_outcome(allocator, owned, &problem_view,
                               &prefix_digest, &decision, NULL, 0u, output,
                               error);
        goto cleanup;
    }
    if (state.view.self_pair != 0u &&
        state.view.disposition == QL_LOOP_PROOF_QUERY_READY &&
        state.view.strategy != QL_LOOP_PROOF_STRATEGY_STRUCTURAL_INDUCTION) {
        set_diagnostic(&decision,
                       "exact self-pairs are the reflexivity fast path's territory, not this method's");
        status = build_outcome(allocator, owned, &problem_view,
                               &prefix_digest, &decision, NULL, 0u, output,
                               error);
        goto cleanup;
    }

    state.allocator = allocator;
    state.instance = owned;
    state.context = context;
    state.query = query;
    state.decision = &decision;
    status = parse_state_inventory(ql_loop_proof_query_prefix_artifact(query),
                                   state.variables,
                                   QL_CHCPDR_MAX_STATE_VARIABLES,
                                   &state.variable_count, error);
    if (status == QL_STATUS_OK) {
        status = require_definitions(
            ql_loop_proof_query_prefix_artifact(query), state.variables,
            state.variable_count, error);
    }
    if (status == QL_STATUS_TYPE_MISMATCH) {
        set_diagnostic(&decision, error->message);
        status = build_outcome(allocator, owned, &problem_view,
                               &prefix_digest, &decision, NULL, 0u, output,
                               error);
        goto cleanup;
    }
    if (status != QL_STATUS_OK) {
        goto cleanup;
    }

    state.frame_capacity = (size_t)owned->max_frames + 2u;
    state.lemmas = allocator->allocate(
        allocator->user_data, (size_t)owned->max_lemmas *
                                  sizeof(*state.lemmas));
    state.member = allocator->allocate(
        allocator->user_data,
        (size_t)owned->max_lemmas * state.frame_capacity);
    if (state.lemmas == NULL || state.member == NULL) {
        ql_error_set(error, QL_STATUS_OUT_OF_MEMORY, NULL);
        status = QL_STATUS_OUT_OF_MEMORY;
        goto cleanup;
    }
    memset(state.member, 0,
           (size_t)owned->max_lemmas * state.frame_capacity);
    status = build_fixed_formulas(&state, error);
    if (status != QL_STATUS_OK) {
        goto cleanup;
    }
    state_ready = 1u;

    {
        size_t first_seed = 0u;
        size_t seed_count = 0u;
        chcpdr_outcome_kind outcome_kind = QL_CHCPDR_RESULT_STOPPED;

        status = build_seed_lemmas(&state, &first_seed, &seed_count, error);
        if (status != QL_STATUS_OK) {
            goto cleanup;
        }
        status = pdr_search(&state, first_seed, seed_count, &outcome_kind,
                            &invariant_frame, error);
        if (status != QL_STATUS_OK) {
            goto cleanup;
        }
        decision.lemma_count = state.lemma_count;
        decision.queries_used = state.queries_used;

        if (outcome_kind == QL_CHCPDR_RESULT_TRACE) {
            set_diagnostic(&decision,
                           "a bad state is reachable in the relational transition system; this method does not concretize traces, so refutation belongs to search.bounded-symbolic or refute.concrete-differential");
        } else if (outcome_kind == QL_CHCPDR_RESULT_STOPPED) {
            set_diagnostic(&decision,
                           "the frame, lemma, query, or time budget was exhausted before a fixpoint");
        } else {
            uint32_t verified = 0u;
            uint32_t inhabited = 0u;
            size_t index;
            for (index = 0u; index < state.lemma_count; ++index) {
                if (state.member[index * state.frame_capacity +
                                 invariant_frame]) {
                    ++decision.invariant_lemma_count;
                }
            }
            status = verify_invariant(&state, invariant_frame, &verified,
                                      error);
            if (status != QL_STATUS_OK) {
                goto cleanup;
            }
            decision.queries_used = state.queries_used;
            if (verified == 0u) {
                QL_LOGE(QL_CHCPDR_CATEGORY,
                        "a PDR fixpoint failed its own re-verification");
                set_diagnostic(&decision,
                               "the fixpoint did not survive independent re-verification; nothing is claimed");
                status = build_outcome(allocator, owned, &problem_view,
                                       &prefix_digest, &decision, &state,
                                       invariant_frame, output, error);
                goto cleanup;
            }
            decision.invariant_verified = 1u;
            status = check_domain(&state, &inhabited, error);
            if (status != QL_STATUS_OK) {
                goto cleanup;
            }
            decision.queries_used = state.queries_used;
            if (inhabited == 0u) {
                set_diagnostic(&decision,
                               "the invariant is verified but the comparison domain was not shown inhabited, so the claim would be vacuous");
            } else if (owned->unsat_promotion !=
                       QL_CHCPDR_PROMOTION_TRUSTED_BACKEND) {
                set_diagnostic(&decision,
                               "an inductive invariant is verified; promotion requires the explicit trusted-backend policy");
            } else if (state.view.nonvacuity_eligible == 0u ||
                       state.view.has_ub_guard_or_terminator != 0u ||
                       binding_match == 0u ||
                       loop_options.precondition_is_true == 0u) {
                set_diagnostic(&decision,
                               "an inductive invariant is verified, but the whole-IR non-vacuity, binding, or precondition gate is not satisfied");
            } else {
                decision.verdict =
                    proved_verdict_for(problem_view.contract.relation);
                decision.evidence_class = QL_EVIDENCE_PROOF;
                set_diagnostic(&decision,
                               "the verified inductive invariant aligns the guards and equates the exit observable for every reachable synchronized state");
            }
        }
    }

    status = build_outcome(allocator, owned, &problem_view, &prefix_digest,
                           &decision, state_ready != 0u ? &state : NULL,
                           invariant_frame, output, error);

cleanup:
    if (state_ready != 0u || state.lemmas != NULL || state.member != NULL) {
        state_dispose(&state);
    }
    ql_loop_proof_query_destroy(query);
    chcpdr_side_dispose(&left);
    chcpdr_side_dispose(&right);
    ql_problem_release(problem);
    if (status == QL_STATUS_OK) {
        ql_error_clear(error);
    }
    return status;
}

static const ql_method_v1 chcpdr_method = {
    sizeof(ql_method_v1),
    QL_ABI_VERSION,
    QL_CHCPDR_METHOD_NAME,
    "Property-directed reachability over the serialized relational loop pair",
    QL_ARTIFACT_KIND_OUTCOME,
    QL_METHOD_PROOF_PRODUCER | QL_METHOD_CACHEABLE,
    1u,
    1u,
    chcpdr_create,
    chcpdr_validate,
    chcpdr_run,
    chcpdr_destroy,
    { NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL }
};

static const ql_proof_method_v1 chcpdr_descriptor = {
    sizeof(ql_proof_method_v1),
    QL_ABI_VERSION,
    QL_PROOF_METHOD_FAMILY_CHC_PDR,
    &chcpdr_method,
    chcpdr_capability,
    { NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL }
};

const ql_method_v1 *QL_CALL ql_chcpdr_method(void) {
    return &chcpdr_method;
}

const ql_proof_method_v1 *QL_CALL ql_chcpdr_proof_method(void) {
    return &chcpdr_descriptor;
}

ql_status QL_CALL ql_register_chcpdr_method(ql_registry *registry,
                                            ql_error *error) {
    ql_status status = ql_registry_register(registry, &chcpdr_method, error);
    if (status != QL_STATUS_OK) {
        return status;
    }
    return ql_registry_register_proof_method(registry, &chcpdr_descriptor,
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

ql_status QL_CALL ql_chcpdr_outcome_read(const ql_artifact *artifact,
                                         ql_chcpdr_outcome_view_v1 *view,
                                         ql_error *error) {
    ql_artifact_view artifact_view;
    yyjson_doc *document = NULL;
    yyjson_read_err read_error;
    yyjson_val *root;
    yyjson_val *search_object;
    yyjson_val *invariant_array;
    yyjson_val *trust;
    const char *text;
    ql_status status = QL_STATUS_OK;

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
    memset(&artifact_view, 0, sizeof(artifact_view));
    artifact_view.struct_size = sizeof(artifact_view);
    status = ql_artifact_get_view(artifact, &artifact_view, error);
    if (status != QL_STATUS_OK) {
        return status;
    }
    if (strcmp(artifact_view.kind, QL_ARTIFACT_KIND_OUTCOME) != 0 ||
        artifact_view.schema_version != QL_CHCPDR_OUTCOME_SCHEMA_VERSION) {
        ql_error_set(error, QL_STATUS_TYPE_MISMATCH,
                     "artifact is not a schema v1 quodlibet.outcome");
        return QL_STATUS_TYPE_MISMATCH;
    }
    document = yyjson_read_opts((char *)(uintptr_t)artifact_view.data,
                                artifact_view.size, 0u, NULL, &read_error);
    if (document == NULL) {
        ql_error_set(error, QL_STATUS_PARSE_ERROR,
                     "invalid outcome JSON at byte %zu: %s", read_error.pos,
                     read_error.msg != NULL ? read_error.msg : "parse error");
        return QL_STATUS_PARSE_ERROR;
    }
    memset(view, 0, sizeof(*view));
    view->struct_size = sizeof(*view);
    view->schema_version = QL_CHCPDR_OUTCOME_SCHEMA_VERSION;
    root = yyjson_doc_get_root(document);
    search_object = yyjson_obj_get(root, "search");
    invariant_array = yyjson_obj_get(root, "invariant");
    trust = yyjson_obj_get(root, "trust");
    if (!yyjson_is_obj(root) || !yyjson_is_obj(search_object) ||
        !yyjson_is_arr(invariant_array) || !yyjson_is_obj(trust)) {
        ql_error_set(error, QL_STATUS_SCHEMA_MISMATCH,
                     "outcome JSON does not match schema version 1");
        status = QL_STATUS_SCHEMA_MISMATCH;
        goto cleanup;
    }
    text = yyjson_get_str(yyjson_obj_get(root, "method"));
    if (text == NULL || strcmp(text, QL_CHCPDR_METHOD_NAME) != 0) {
        ql_error_set(error, QL_STATUS_TYPE_MISMATCH,
                     "outcome was not produced by %s", QL_CHCPDR_METHOD_NAME);
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
    view->invariant_verified =
        yyjson_get_bool(yyjson_obj_get(search_object, "invariant_verified"))
            ? 1u
            : 0u;
    view->frames_used = (uint32_t)yyjson_get_uint(
        yyjson_obj_get(search_object, "frames_used"));
    view->lemma_count =
        yyjson_get_uint(yyjson_obj_get(search_object, "lemma_count"));
    view->invariant_lemma_count = yyjson_arr_size(invariant_array);
    view->queries_used =
        yyjson_get_uint(yyjson_obj_get(search_object, "queries_used"));
    (void)read_digest_field(root, "problem_digest", &view->problem_digest);
    (void)read_digest_field(root, "cache_key", &view->cache_key);
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
