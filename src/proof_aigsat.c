#include "quodlibet/proof_aigsat.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "quodlibet/c_lower.h"
#include "quodlibet/log.h"
#include "quodlibet/problem.h"
#include "quodlibet/product.h"
#include "quodlibet/replay.h"
#include "quodlibet/solver.h"

#include "yyjson.h"

/* Included ahead of use so the runner interface is compiled from the day it is
   agreed, not from the day the extraction lands. */
#include "process_runner.h"

/* --- Values --------------------------------------------------------------- */

/* A blasted term. A Boolean is one bit; nothing else distinguishes it from a
   one-bit vector except that the grammar keeps the two sorts apart, and a sort
   confusion is a refusal rather than a silent coercion. */
typedef struct blast_value {
    uint32_t is_bool;
    uint32_t width;
    ql_aig_lit bits[QL_AIG_MAX_BIT_WIDTH];
} blast_value;

typedef struct blast_symbol {
    char *name;
    size_t name_size;
    blast_value value;
    uint32_t is_declared;
    uint32_t first_input;
} blast_symbol;

struct ql_aig_blast {
    ql_allocator allocator;
    blast_symbol *symbols;
    size_t symbol_count;
    size_t symbol_capacity;
    /* Open addressing over the symbol table; a slot holds index + 1. */
    uint32_t *table;
    size_t table_size;
    size_t declared_count;
    size_t defined_count;
    size_t assertion_count;
    ql_aig_lit root;
};

typedef struct blast_state {
    ql_aig *aig;
    ql_aig_blast *blast;
    const char *text;
    size_t size;
    size_t cursor;
    ql_error *error;
} blast_state;

static const ql_allocator *select_allocator(const ql_allocator *allocator) {
    return allocator == NULL ? ql_default_allocator() : allocator;
}

static ql_status refuse(blast_state *state, const char *what) {
    ql_error_set(state->error, QL_STATUS_TYPE_MISMATCH,
                 "the AIG bit-blaster does not accept %s at byte %zu; this "
                 "grammar is exactly what ql_smt2_builder emits and nothing "
                 "outside it is guessed at",
                 what, state->cursor);
    return QL_STATUS_TYPE_MISMATCH;
}

/* --- Symbol table --------------------------------------------------------- */

static size_t hash_name(const char *name, size_t size) {
    uint64_t value = UINT64_C(0xcbf29ce484222325);
    size_t index;

    for (index = 0u; index < size; ++index) {
        value ^= (uint64_t)(unsigned char)name[index];
        value *= UINT64_C(0x100000001b3);
    }
    return (size_t)value;
}

static void table_insert(ql_aig_blast *blast, size_t symbol) {
    const size_t mask = blast->table_size - 1u;
    size_t slot = hash_name(blast->symbols[symbol].name,
                            blast->symbols[symbol].name_size) &
                  mask;

    while (blast->table[slot] != 0u) {
        slot = (slot + 1u) & mask;
    }
    blast->table[slot] = (uint32_t)(symbol + 1u);
}

static ql_status table_grow(ql_aig_blast *blast, ql_error *error) {
    const size_t capacity =
        blast->table_size == 0u ? 512u : blast->table_size * 2u;
    uint32_t *table = blast->allocator.allocate(blast->allocator.user_data,
                                                capacity * sizeof(*table));
    uint32_t *previous = blast->table;
    size_t index;

    if (table == NULL) {
        ql_error_set(error, QL_STATUS_OUT_OF_MEMORY, NULL);
        return QL_STATUS_OUT_OF_MEMORY;
    }
    memset(table, 0, capacity * sizeof(*table));
    blast->allocator.deallocate(blast->allocator.user_data, previous);
    blast->table = table;
    blast->table_size = capacity;
    for (index = 0u; index < blast->symbol_count; ++index) {
        table_insert(blast, index);
    }
    return QL_STATUS_OK;
}

static blast_symbol *symbol_find(const ql_aig_blast *blast, const char *name,
                                 size_t size) {
    size_t mask;
    size_t slot;

    if (blast->table_size == 0u) {
        return NULL;
    }
    mask = blast->table_size - 1u;
    slot = hash_name(name, size) & mask;
    while (blast->table[slot] != 0u) {
        blast_symbol *symbol = &blast->symbols[blast->table[slot] - 1u];
        if (symbol->name_size == size &&
            memcmp(symbol->name, name, size) == 0) {
            return symbol;
        }
        slot = (slot + 1u) & mask;
    }
    return NULL;
}

static ql_status symbol_add(blast_state *state, const char *name, size_t size,
                            blast_symbol **output) {
    ql_aig_blast *blast = state->blast;
    blast_symbol *symbol;
    ql_status status;

    if (symbol_find(blast, name, size) != NULL) {
        ql_error_set(state->error, QL_STATUS_ALREADY_EXISTS,
                     "the query declares the symbol '%.*s' twice",
                     (int)size, name);
        return QL_STATUS_ALREADY_EXISTS;
    }
    if (blast->symbol_count == blast->symbol_capacity) {
        const size_t capacity =
            blast->symbol_capacity == 0u ? 256u : blast->symbol_capacity * 2u;
        blast_symbol *grown = blast->allocator.reallocate(
            blast->allocator.user_data, blast->symbols,
            capacity * sizeof(*grown));
        if (grown == NULL) {
            ql_error_set(state->error, QL_STATUS_OUT_OF_MEMORY, NULL);
            return QL_STATUS_OUT_OF_MEMORY;
        }
        blast->symbols = grown;
        blast->symbol_capacity = capacity;
    }
    symbol = &blast->symbols[blast->symbol_count];
    memset(symbol, 0, sizeof(*symbol));
    symbol->name = blast->allocator.allocate(blast->allocator.user_data,
                                             size + 1u);
    if (symbol->name == NULL) {
        ql_error_set(state->error, QL_STATUS_OUT_OF_MEMORY, NULL);
        return QL_STATUS_OUT_OF_MEMORY;
    }
    memcpy(symbol->name, name, size);
    symbol->name[size] = '\0';
    symbol->name_size = size;
    ++blast->symbol_count;
    if (blast->table_size == 0u ||
        blast->symbol_count * 2u >= blast->table_size) {
        status = table_grow(blast, state->error);
        if (status != QL_STATUS_OK) {
            return status;
        }
    } else {
        table_insert(blast, blast->symbol_count - 1u);
    }
    *output = symbol;
    return QL_STATUS_OK;
}

/* --- Scanning ------------------------------------------------------------- */

static void skip_space(blast_state *state) {
    while (state->cursor < state->size) {
        const char ch = state->text[state->cursor];
        if (ch == ';') {
            while (state->cursor < state->size &&
                   state->text[state->cursor] != '\n') {
                ++state->cursor;
            }
            continue;
        }
        if (ch != ' ' && ch != '\t' && ch != '\r' && ch != '\n') {
            return;
        }
        ++state->cursor;
    }
}

static int at_end(blast_state *state) {
    skip_space(state);
    return state->cursor >= state->size;
}

static int peek_is(blast_state *state, char ch) {
    skip_space(state);
    return state->cursor < state->size && state->text[state->cursor] == ch;
}

static ql_status expect(blast_state *state, char ch) {
    if (!peek_is(state, ch)) {
        return refuse(state, ch == '(' ? "a missing opening parenthesis"
                                       : "a missing closing parenthesis");
    }
    ++state->cursor;
    return QL_STATUS_OK;
}

/* Reads one atom. Parentheses terminate it and are never part of it, which is
   all the tokenizing this grammar needs. */
static ql_status read_atom(blast_state *state, const char **start,
                           size_t *length) {
    size_t begin;

    skip_space(state);
    begin = state->cursor;
    while (state->cursor < state->size) {
        const char ch = state->text[state->cursor];
        if (ch == '(' || ch == ')' || ch == ' ' || ch == '\t' ||
            ch == '\r' || ch == '\n' || ch == ';') {
            break;
        }
        ++state->cursor;
    }
    if (state->cursor == begin) {
        return refuse(state, "an empty token where an identifier was expected");
    }
    *start = state->text + begin;
    *length = state->cursor - begin;
    return QL_STATUS_OK;
}

static int atom_is(const char *text, size_t size, const char *expected) {
    const size_t expected_size = strlen(expected);
    return size == expected_size && memcmp(text, expected, size) == 0;
}

static ql_status read_number(blast_state *state, uint32_t *output) {
    const char *text;
    size_t size;
    size_t index;
    uint64_t value = 0u;
    ql_status status = read_atom(state, &text, &size);

    if (status != QL_STATUS_OK) {
        return status;
    }
    for (index = 0u; index < size; ++index) {
        if (text[index] < '0' || text[index] > '9') {
            return refuse(state, "a non-numeral where an index was expected");
        }
        value = value * 10u + (uint64_t)(text[index] - '0');
        if (value > UINT32_MAX) {
            return refuse(state, "an index that does not fit in 32 bits");
        }
    }
    *output = (uint32_t)value;
    return QL_STATUS_OK;
}

/* --- Constants ------------------------------------------------------------ */

static void bits_set(ql_aig_lit *bits, uint32_t width, uint32_t position,
                     int value) {
    if (position < width) {
        bits[position] = value != 0 ? QL_AIG_LIT_TRUE : QL_AIG_LIT_FALSE;
    }
}

/* Decimal to bits by repeated doubling, so a numeral wider than any machine
   integer still lands correctly instead of overflowing a parse. */
static ql_status decimal_to_bits(blast_state *state, const char *text,
                                 size_t size, uint32_t width,
                                 ql_aig_lit *bits) {
    uint8_t value[QL_AIG_MAX_BIT_WIDTH];
    size_t index;
    uint32_t position;

    memset(value, 0, sizeof(value));
    if (size == 0u) {
        return refuse(state, "an empty numeral");
    }
    for (index = 0u; index < size; ++index) {
        uint32_t carry;
        if (text[index] < '0' || text[index] > '9') {
            return refuse(state, "a non-numeral inside a bit-vector literal");
        }
        carry = (uint32_t)(text[index] - '0');
        for (position = 0u; position < width; ++position) {
            const uint32_t product = (uint32_t)value[position] * 10u + carry;
            value[position] = (uint8_t)(product & 1u);
            carry = product >> 1;
        }
        /* Bits above the width are discarded, which is the modulo-2^width
           reading SMT-LIB gives a numeral literal. */
    }
    for (position = 0u; position < width; ++position) {
        bits[position] = value[position] != 0u ? QL_AIG_LIT_TRUE
                                               : QL_AIG_LIT_FALSE;
    }
    return QL_STATUS_OK;
}

/* --- Sorts ---------------------------------------------------------------- */

/* Reads `Bool` or `(_ BitVec N)`. An Array sort is refused rather than
   approximated: the first cut of this backend is scalar. */
static ql_status parse_sort(blast_state *state, uint32_t *is_bool,
                            uint32_t *width) {
    const char *text;
    size_t size;
    ql_status status;

    if (peek_is(state, '(')) {
        ++state->cursor;
        status = read_atom(state, &text, &size);
        if (status != QL_STATUS_OK) {
            return status;
        }
        if (atom_is(text, size, "Array")) {
            return refuse(state,
                          "an Array sort; the scalar miter has no memory model");
        }
        if (!atom_is(text, size, "_")) {
            return refuse(state, "a sort outside Bool and (_ BitVec N)");
        }
        status = read_atom(state, &text, &size);
        if (status != QL_STATUS_OK) {
            return status;
        }
        if (!atom_is(text, size, "BitVec")) {
            return refuse(state, "a sort outside Bool and (_ BitVec N)");
        }
        status = read_number(state, width);
        if (status != QL_STATUS_OK) {
            return status;
        }
        if (*width == 0u || *width > QL_AIG_MAX_BIT_WIDTH) {
            return refuse(state, "a bit-vector wider than this blaster builds");
        }
        *is_bool = 0u;
        return expect(state, ')');
    }
    status = read_atom(state, &text, &size);
    if (status != QL_STATUS_OK) {
        return status;
    }
    if (atom_is(text, size, "Bool")) {
        *is_bool = 1u;
        *width = 1u;
        return QL_STATUS_OK;
    }
    if (atom_is(text, size, "Array")) {
        return refuse(state, "an Array sort; the scalar miter has no memory model");
    }
    return refuse(state, "a sort outside Bool and (_ BitVec N)");
}

/* --- Terms ---------------------------------------------------------------- */

static ql_status parse_term(blast_state *state, blast_value *output);

static ql_status require_bool(blast_state *state, const blast_value *value) {
    if (value->is_bool == 0u) {
        return refuse(state, "a bit-vector where a Boolean was required");
    }
    return QL_STATUS_OK;
}

static ql_status require_bv(blast_state *state, const blast_value *value) {
    if (value->is_bool != 0u) {
        return refuse(state, "a Boolean where a bit-vector was required");
    }
    return QL_STATUS_OK;
}

static ql_status require_same_width(blast_state *state,
                                    const blast_value *left,
                                    const blast_value *right) {
    if (left->is_bool != right->is_bool || left->width != right->width) {
        return refuse(state, "operands of two different sorts");
    }
    return QL_STATUS_OK;
}

static void set_bool(blast_value *value, ql_aig_lit literal) {
    value->is_bool = 1u;
    value->width = 1u;
    value->bits[0] = literal;
}

typedef ql_status (QL_CALL *bv_binary_fn)(ql_aig *, const ql_aig_lit *,
                                          const ql_aig_lit *, uint32_t,
                                          ql_aig_lit *, ql_error *);
typedef ql_status (QL_CALL *bv_predicate_fn)(ql_aig *, const ql_aig_lit *,
                                             const ql_aig_lit *, uint32_t,
                                             ql_aig_lit *, ql_error *);

/* The circuit layer takes a carry in and hands two results out of a division,
   because a lowering wants both. The grammar's operators want neither, so
   these four adapters narrow the shapes rather than widen the circuit API. */
static ql_status QL_CALL bv_add_wrap(ql_aig *aig, const ql_aig_lit *left,
                                     const ql_aig_lit *right, uint32_t width,
                                     ql_aig_lit *output, ql_error *error) {
    return ql_aig_bv_add(aig, left, right, QL_AIG_LIT_FALSE, width, output,
                         NULL, error);
}

static ql_status QL_CALL bv_udiv(ql_aig *aig, const ql_aig_lit *left,
                                 const ql_aig_lit *right, uint32_t width,
                                 ql_aig_lit *output, ql_error *error) {
    return ql_aig_bv_udivrem(aig, left, right, width, output, NULL, error);
}

static ql_status QL_CALL bv_urem(ql_aig *aig, const ql_aig_lit *left,
                                 const ql_aig_lit *right, uint32_t width,
                                 ql_aig_lit *output, ql_error *error) {
    return ql_aig_bv_udivrem(aig, left, right, width, NULL, output, error);
}

static ql_status QL_CALL bv_sdiv(ql_aig *aig, const ql_aig_lit *left,
                                 const ql_aig_lit *right, uint32_t width,
                                 ql_aig_lit *output, ql_error *error) {
    return ql_aig_bv_sdivrem(aig, left, right, width, output, NULL, error);
}

static ql_status QL_CALL bv_srem(ql_aig *aig, const ql_aig_lit *left,
                                 const ql_aig_lit *right, uint32_t width,
                                 ql_aig_lit *output, ql_error *error) {
    return ql_aig_bv_sdivrem(aig, left, right, width, NULL, output, error);
}

static ql_status parse_binary_bv(blast_state *state, bv_binary_fn operation,
                                 blast_value *output) {
    blast_value left;
    blast_value right;
    ql_status status = parse_term(state, &left);

    if (status != QL_STATUS_OK) {
        return status;
    }
    status = parse_term(state, &right);
    if (status != QL_STATUS_OK) {
        return status;
    }
    status = require_bv(state, &left);
    if (status == QL_STATUS_OK) {
        status = require_same_width(state, &left, &right);
    }
    if (status != QL_STATUS_OK) {
        return status;
    }
    output->is_bool = 0u;
    output->width = left.width;
    return operation(state->aig, left.bits, right.bits, left.width,
                     output->bits, state->error);
}

static ql_status parse_predicate_bv(blast_state *state,
                                    bv_predicate_fn operation, int swap,
                                    int invert, blast_value *output) {
    blast_value left;
    blast_value right;
    ql_aig_lit result = QL_AIG_LIT_FALSE;
    ql_status status = parse_term(state, &left);

    if (status != QL_STATUS_OK) {
        return status;
    }
    status = parse_term(state, &right);
    if (status != QL_STATUS_OK) {
        return status;
    }
    status = require_bv(state, &left);
    if (status == QL_STATUS_OK) {
        status = require_same_width(state, &left, &right);
    }
    if (status != QL_STATUS_OK) {
        return status;
    }
    status = swap != 0
                 ? operation(state->aig, right.bits, left.bits, left.width,
                             &result, state->error)
                 : operation(state->aig, left.bits, right.bits, left.width,
                             &result, state->error);
    if (status != QL_STATUS_OK) {
        return status;
    }
    set_bool(output, invert != 0 ? ql_aig_not(result) : result);
    return QL_STATUS_OK;
}

/* `and` and `or` are variadic in the emitted text, including the empty case
   that the reachability disjunction can produce. */
static ql_status parse_variadic(blast_state *state, int is_and,
                                blast_value *output) {
    ql_aig_lit accumulator = is_and != 0 ? QL_AIG_LIT_TRUE : QL_AIG_LIT_FALSE;
    ql_status status;

    while (!peek_is(state, ')')) {
        blast_value operand;
        if (at_end(state)) {
            return refuse(state, "a term that ends before its closing parenthesis");
        }
        status = parse_term(state, &operand);
        if (status != QL_STATUS_OK) {
            return status;
        }
        status = require_bool(state, &operand);
        if (status != QL_STATUS_OK) {
            return status;
        }
        status = is_and != 0
                     ? ql_aig_and(state->aig, accumulator, operand.bits[0],
                                  &accumulator, state->error)
                     : ql_aig_or(state->aig, accumulator, operand.bits[0],
                                 &accumulator, state->error);
        if (status != QL_STATUS_OK) {
            return status;
        }
    }
    set_bool(output, accumulator);
    return QL_STATUS_OK;
}

static ql_status parse_equality(blast_state *state, int invert,
                                blast_value *output) {
    blast_value left;
    blast_value right;
    ql_aig_lit result = QL_AIG_LIT_FALSE;
    ql_status status = parse_term(state, &left);

    if (status != QL_STATUS_OK) {
        return status;
    }
    status = parse_term(state, &right);
    if (status != QL_STATUS_OK) {
        return status;
    }
    status = require_same_width(state, &left, &right);
    if (status != QL_STATUS_OK) {
        return status;
    }
    status = left.is_bool != 0u
                 ? ql_aig_xnor(state->aig, left.bits[0], right.bits[0],
                               &result, state->error)
                 : ql_aig_bv_eq(state->aig, left.bits, right.bits, left.width,
                                &result, state->error);
    if (status != QL_STATUS_OK) {
        return status;
    }
    set_bool(output, invert != 0 ? ql_aig_not(result) : result);
    return QL_STATUS_OK;
}

static ql_status parse_indexed_application(blast_state *state,
                                           blast_value *output) {
    const char *text;
    size_t size;
    blast_value operand;
    uint32_t first;
    uint32_t second;
    ql_status status;

    /* The cursor sits on the inner '(' of `((_ op i ...) arg)`. */
    status = expect(state, '(');
    if (status != QL_STATUS_OK) {
        return status;
    }
    status = read_atom(state, &text, &size);
    if (status != QL_STATUS_OK) {
        return status;
    }
    if (!atom_is(text, size, "_")) {
        return refuse(state, "an application head that is not an indexed identifier");
    }
    status = read_atom(state, &text, &size);
    if (status != QL_STATUS_OK) {
        return status;
    }
    if (atom_is(text, size, "zero_extend") ||
        atom_is(text, size, "sign_extend")) {
        const int is_zero = text[0] == 'z';
        status = read_number(state, &first);
        if (status == QL_STATUS_OK) {
            status = expect(state, ')');
        }
        if (status == QL_STATUS_OK) {
            status = parse_term(state, &operand);
        }
        if (status == QL_STATUS_OK) {
            status = require_bv(state, &operand);
        }
        if (status != QL_STATUS_OK) {
            return status;
        }
        if (first > QL_AIG_MAX_BIT_WIDTH - operand.width) {
            return refuse(state, "an extension past the maximum width");
        }
        output->is_bool = 0u;
        output->width = operand.width + first;
        return is_zero != 0
                   ? ql_aig_bv_zext(state->aig, operand.bits, operand.width,
                                    output->width, output->bits, state->error)
                   : ql_aig_bv_sext(state->aig, operand.bits, operand.width,
                                    output->width, output->bits,
                                    state->error);
    }
    if (atom_is(text, size, "extract")) {
        status = read_number(state, &first);
        if (status == QL_STATUS_OK) {
            status = read_number(state, &second);
        }
        if (status == QL_STATUS_OK) {
            status = expect(state, ')');
        }
        if (status == QL_STATUS_OK) {
            status = parse_term(state, &operand);
        }
        if (status == QL_STATUS_OK) {
            status = require_bv(state, &operand);
        }
        if (status != QL_STATUS_OK) {
            return status;
        }
        if (second != 0u) {
            /* The emitter only ever extracts a low slice. Anything else would
               need a shift this blaster does not silently insert. */
            return refuse(state, "an extract whose low index is not zero");
        }
        if (first >= operand.width) {
            return refuse(state, "an extract past the end of its operand");
        }
        output->is_bool = 0u;
        output->width = first + 1u;
        return ql_aig_bv_trunc(state->aig, operand.bits, operand.width,
                               output->width, output->bits, state->error);
    }
    return refuse(state, "an indexed operator outside zero_extend, sign_extend, and extract");
}

static ql_status parse_atom_term(blast_state *state, blast_value *output) {
    const char *text;
    size_t size;
    const blast_symbol *symbol;
    ql_status status = read_atom(state, &text, &size);

    if (status != QL_STATUS_OK) {
        return status;
    }
    if (atom_is(text, size, "true")) {
        set_bool(output, QL_AIG_LIT_TRUE);
        return QL_STATUS_OK;
    }
    if (atom_is(text, size, "false")) {
        set_bool(output, QL_AIG_LIT_FALSE);
        return QL_STATUS_OK;
    }
    if (size > 2u && text[0] == '#' && text[1] == 'b') {
        const uint32_t width = (uint32_t)(size - 2u);
        uint32_t index;
        if (width > QL_AIG_MAX_BIT_WIDTH) {
            return refuse(state, "a binary literal wider than this blaster builds");
        }
        output->is_bool = 0u;
        output->width = width;
        for (index = 0u; index < width; ++index) {
            const char ch = text[2u + index];
            if (ch != '0' && ch != '1') {
                return refuse(state, "a binary literal with a non-binary digit");
            }
            /* Most significant bit first in the text, least significant bit
               first in a word. */
            bits_set(output->bits, width, width - 1u - index, ch == '1');
        }
        return QL_STATUS_OK;
    }
    symbol = symbol_find(state->blast, text, size);
    if (symbol == NULL) {
        ql_error_set(state->error, QL_STATUS_NOT_FOUND,
                     "the query uses the undeclared symbol '%.*s'", (int)size,
                     text);
        return QL_STATUS_NOT_FOUND;
    }
    *output = symbol->value;
    return QL_STATUS_OK;
}

static ql_status parse_term(blast_state *state, blast_value *output) {
    const char *text;
    size_t size;
    blast_value first;
    blast_value second;
    blast_value third;
    ql_aig_lit result = QL_AIG_LIT_FALSE;
    ql_status status;

    if (at_end(state)) {
        return refuse(state, "a query that ends where a term was expected");
    }
    if (!peek_is(state, '(')) {
        return parse_atom_term(state, output);
    }
    ++state->cursor;
    if (peek_is(state, '(')) {
        status = parse_indexed_application(state, output);
        if (status != QL_STATUS_OK) {
            return status;
        }
        return expect(state, ')');
    }
    status = read_atom(state, &text, &size);
    if (status != QL_STATUS_OK) {
        return status;
    }

    if (atom_is(text, size, "_")) {
        /* `(_ bvDECIMAL WIDTH)` */
        uint32_t width;
        status = read_atom(state, &text, &size);
        if (status != QL_STATUS_OK) {
            return status;
        }
        if (size <= 2u || memcmp(text, "bv", 2u) != 0) {
            return refuse(state, "an indexed constant that is not (_ bvN W)");
        }
        {
            const char *digits = text + 2;
            const size_t digit_count = size - 2u;
            status = read_number(state, &width);
            if (status != QL_STATUS_OK) {
                return status;
            }
            if (width == 0u || width > QL_AIG_MAX_BIT_WIDTH) {
                return refuse(state, "a constant wider than this blaster builds");
            }
            output->is_bool = 0u;
            output->width = width;
            status = decimal_to_bits(state, digits, digit_count, width,
                                     output->bits);
        }
        if (status != QL_STATUS_OK) {
            return status;
        }
        return expect(state, ')');
    }
    if (atom_is(text, size, "not")) {
        status = parse_term(state, &first);
        if (status == QL_STATUS_OK) {
            status = require_bool(state, &first);
        }
        if (status != QL_STATUS_OK) {
            return status;
        }
        set_bool(output, ql_aig_not(first.bits[0]));
        return expect(state, ')');
    }
    if (atom_is(text, size, "and") || atom_is(text, size, "or")) {
        status = parse_variadic(state, text[0] == 'a', output);
        if (status != QL_STATUS_OK) {
            return status;
        }
        return expect(state, ')');
    }
    if (atom_is(text, size, "=>")) {
        status = parse_term(state, &first);
        if (status == QL_STATUS_OK) {
            status = parse_term(state, &second);
        }
        if (status == QL_STATUS_OK) {
            status = require_bool(state, &first);
        }
        if (status == QL_STATUS_OK) {
            status = require_bool(state, &second);
        }
        if (status == QL_STATUS_OK) {
            status = ql_aig_implies(state->aig, first.bits[0], second.bits[0],
                                    &result, state->error);
        }
        if (status != QL_STATUS_OK) {
            return status;
        }
        set_bool(output, result);
        return expect(state, ')');
    }
    if (atom_is(text, size, "xor")) {
        status = parse_term(state, &first);
        if (status == QL_STATUS_OK) {
            status = parse_term(state, &second);
        }
        if (status == QL_STATUS_OK) {
            status = require_bool(state, &first);
        }
        if (status == QL_STATUS_OK) {
            status = require_bool(state, &second);
        }
        if (status == QL_STATUS_OK) {
            status = ql_aig_xor(state->aig, first.bits[0], second.bits[0],
                                &result, state->error);
        }
        if (status != QL_STATUS_OK) {
            return status;
        }
        set_bool(output, result);
        return expect(state, ')');
    }
    if (atom_is(text, size, "=") || atom_is(text, size, "distinct")) {
        status = parse_equality(state, text[0] == 'd', output);
        if (status != QL_STATUS_OK) {
            return status;
        }
        return expect(state, ')');
    }
    if (atom_is(text, size, "ite")) {
        status = parse_term(state, &first);
        if (status == QL_STATUS_OK) {
            status = require_bool(state, &first);
        }
        if (status == QL_STATUS_OK) {
            status = parse_term(state, &second);
        }
        if (status == QL_STATUS_OK) {
            status = parse_term(state, &third);
        }
        if (status == QL_STATUS_OK) {
            status = require_same_width(state, &second, &third);
        }
        if (status != QL_STATUS_OK) {
            return status;
        }
        output->is_bool = second.is_bool;
        output->width = second.width;
        status = second.is_bool != 0u
                     ? ql_aig_mux(state->aig, first.bits[0], second.bits[0],
                                  third.bits[0], &output->bits[0],
                                  state->error)
                     : ql_aig_bv_mux(state->aig, first.bits[0], second.bits,
                                     third.bits, second.width, output->bits,
                                     state->error);
        if (status != QL_STATUS_OK) {
            return status;
        }
        return expect(state, ')');
    }
    if (atom_is(text, size, "bvnot") || atom_is(text, size, "bvneg")) {
        const int is_not = atom_is(text, size, "bvnot");
        status = parse_term(state, &first);
        if (status == QL_STATUS_OK) {
            status = require_bv(state, &first);
        }
        if (status != QL_STATUS_OK) {
            return status;
        }
        output->is_bool = 0u;
        output->width = first.width;
        status = is_not != 0
                     ? ql_aig_bv_not(state->aig, first.bits, first.width,
                                     output->bits, state->error)
                     : ql_aig_bv_neg(state->aig, first.bits, first.width,
                                     output->bits, state->error);
        if (status != QL_STATUS_OK) {
            return status;
        }
        return expect(state, ')');
    }

    status = QL_STATUS_TYPE_MISMATCH;
    if (atom_is(text, size, "bvadd")) {
        status = parse_binary_bv(state, bv_add_wrap, output);
    } else if (atom_is(text, size, "bvsub")) {
        status = parse_binary_bv(state, ql_aig_bv_sub, output);
    } else if (atom_is(text, size, "bvmul")) {
        status = parse_binary_bv(state, ql_aig_bv_mul, output);
    } else if (atom_is(text, size, "bvand")) {
        status = parse_binary_bv(state, ql_aig_bv_and, output);
    } else if (atom_is(text, size, "bvor")) {
        status = parse_binary_bv(state, ql_aig_bv_or, output);
    } else if (atom_is(text, size, "bvxor")) {
        status = parse_binary_bv(state, ql_aig_bv_xor, output);
    } else if (atom_is(text, size, "bvshl")) {
        status = parse_binary_bv(state, ql_aig_bv_shl, output);
    } else if (atom_is(text, size, "bvlshr")) {
        status = parse_binary_bv(state, ql_aig_bv_lshr, output);
    } else if (atom_is(text, size, "bvashr")) {
        status = parse_binary_bv(state, ql_aig_bv_ashr, output);
    } else if (atom_is(text, size, "bvudiv")) {
        status = parse_binary_bv(state, bv_udiv, output);
    } else if (atom_is(text, size, "bvurem")) {
        status = parse_binary_bv(state, bv_urem, output);
    } else if (atom_is(text, size, "bvsdiv")) {
        status = parse_binary_bv(state, bv_sdiv, output);
    } else if (atom_is(text, size, "bvsrem")) {
        status = parse_binary_bv(state, bv_srem, output);
    } else if (atom_is(text, size, "bvult")) {
        status = parse_predicate_bv(state, ql_aig_bv_ult, 0, 0, output);
    } else if (atom_is(text, size, "bvule")) {
        status = parse_predicate_bv(state, ql_aig_bv_ule, 0, 0, output);
    } else if (atom_is(text, size, "bvugt")) {
        status = parse_predicate_bv(state, ql_aig_bv_ult, 1, 0, output);
    } else if (atom_is(text, size, "bvuge")) {
        status = parse_predicate_bv(state, ql_aig_bv_ule, 1, 0, output);
    } else if (atom_is(text, size, "bvslt")) {
        status = parse_predicate_bv(state, ql_aig_bv_slt, 0, 0, output);
    } else if (atom_is(text, size, "bvsle")) {
        status = parse_predicate_bv(state, ql_aig_bv_sle, 0, 0, output);
    } else if (atom_is(text, size, "bvsgt")) {
        status = parse_predicate_bv(state, ql_aig_bv_slt, 1, 0, output);
    } else if (atom_is(text, size, "bvsge")) {
        status = parse_predicate_bv(state, ql_aig_bv_sle, 1, 0, output);
    } else {
        return refuse(state, "an operator outside the emitted grammar");
    }
    if (status != QL_STATUS_OK) {
        return status;
    }
    return expect(state, ')');
}

/* --- Commands ------------------------------------------------------------- */

static ql_status parse_declare(blast_state *state) {
    const char *name;
    size_t name_size;
    blast_symbol *symbol = NULL;
    uint32_t is_bool = 0u;
    uint32_t width = 0u;
    uint32_t index;
    ql_status status = read_atom(state, &name, &name_size);

    if (status != QL_STATUS_OK) {
        return status;
    }
    status = parse_sort(state, &is_bool, &width);
    if (status != QL_STATUS_OK) {
        return status;
    }
    status = symbol_add(state, name, name_size, &symbol);
    if (status != QL_STATUS_OK) {
        return status;
    }
    symbol->is_declared = 1u;
    symbol->value.is_bool = is_bool;
    symbol->value.width = width;
    symbol->first_input = QL_AIG_LIT_INVALID;
    for (index = 0u; index < width; ++index) {
        ql_aig_lit literal = QL_AIG_LIT_INVALID;
        status = ql_aig_add_input(state->aig, &literal, state->error);
        if (status != QL_STATUS_OK) {
            return status;
        }
        if (index == 0u) {
            symbol->first_input = ql_aig_input_index(state->aig, literal);
        }
        symbol->value.bits[index] = literal;
    }
    ++state->blast->declared_count;
    return expect(state, ')');
}

static ql_status parse_define(blast_state *state) {
    const char *name;
    size_t name_size;
    blast_symbol *symbol = NULL;
    blast_value value;
    uint32_t is_bool = 0u;
    uint32_t width = 0u;
    ql_status status = read_atom(state, &name, &name_size);

    if (status != QL_STATUS_OK) {
        return status;
    }
    /* The emitter only ever defines nullary functions. */
    status = expect(state, '(');
    if (status == QL_STATUS_OK) {
        status = expect(state, ')');
    }
    if (status != QL_STATUS_OK) {
        return refuse(state, "a define-fun with parameters");
    }
    status = parse_sort(state, &is_bool, &width);
    if (status != QL_STATUS_OK) {
        return status;
    }
    status = parse_term(state, &value);
    if (status != QL_STATUS_OK) {
        return status;
    }
    if (value.is_bool != is_bool || value.width != width) {
        return refuse(state, "a definition whose term does not have its declared sort");
    }
    status = symbol_add(state, name, name_size, &symbol);
    if (status != QL_STATUS_OK) {
        return status;
    }
    symbol->value = value;
    symbol->first_input = QL_AIG_LIT_INVALID;
    ++state->blast->defined_count;
    return expect(state, ')');
}

static ql_status parse_assert(blast_state *state) {
    blast_value value;
    ql_status status = parse_term(state, &value);

    if (status != QL_STATUS_OK) {
        return status;
    }
    status = require_bool(state, &value);
    if (status != QL_STATUS_OK) {
        return status;
    }
    status = ql_aig_and(state->aig, state->blast->root, value.bits[0],
                        &state->blast->root, state->error);
    if (status != QL_STATUS_OK) {
        return status;
    }
    ++state->blast->assertion_count;
    return expect(state, ')');
}

static ql_status parse_command(blast_state *state) {
    const char *text;
    size_t size;
    ql_status status = expect(state, '(');

    if (status != QL_STATUS_OK) {
        return status;
    }
    status = read_atom(state, &text, &size);
    if (status != QL_STATUS_OK) {
        return status;
    }
    if (atom_is(text, size, "set-logic")) {
        status = read_atom(state, &text, &size);
        if (status != QL_STATUS_OK) {
            return status;
        }
        return expect(state, ')');
    }
    if (atom_is(text, size, "declare-const")) {
        return parse_declare(state);
    }
    if (atom_is(text, size, "define-fun")) {
        return parse_define(state);
    }
    if (atom_is(text, size, "assert")) {
        return parse_assert(state);
    }
    return refuse(state, "a top-level command outside set-logic, declare-const, define-fun, and assert");
}

/* --- Public API ----------------------------------------------------------- */

void QL_CALL ql_aig_blast_destroy(ql_aig_blast *blast) {
    ql_allocator allocator;
    size_t index;

    if (blast == NULL) {
        return;
    }
    allocator = blast->allocator;
    for (index = 0u; index < blast->symbol_count; ++index) {
        allocator.deallocate(allocator.user_data, blast->symbols[index].name);
    }
    allocator.deallocate(allocator.user_data, blast->symbols);
    allocator.deallocate(allocator.user_data, blast->table);
    allocator.deallocate(allocator.user_data, blast);
}

ql_status QL_CALL ql_aig_blast_smt2(ql_aig *aig,
                                    const ql_artifact *const *parts,
                                    size_t part_count, ql_aig_blast **output,
                                    ql_error *error) {
    const ql_allocator *allocator = select_allocator(NULL);
    ql_aig_blast *blast;
    blast_state state;
    size_t index;
    ql_status status = QL_STATUS_OK;

    if (aig == NULL || output == NULL || (part_count != 0u && parts == NULL)) {
        ql_error_set(error, QL_STATUS_INVALID_ARGUMENT,
                     "graph, parts, and output are required");
        return QL_STATUS_INVALID_ARGUMENT;
    }
    *output = NULL;
    blast = allocator->allocate(allocator->user_data, sizeof(*blast));
    if (blast == NULL) {
        ql_error_set(error, QL_STATUS_OUT_OF_MEMORY, NULL);
        return QL_STATUS_OUT_OF_MEMORY;
    }
    memset(blast, 0, sizeof(*blast));
    blast->allocator = *allocator;
    /* An empty query asserts nothing, which is satisfiable. */
    blast->root = QL_AIG_LIT_TRUE;

    memset(&state, 0, sizeof(state));
    state.aig = aig;
    state.blast = blast;
    state.error = error;
    for (index = 0u; index < part_count; ++index) {
        ql_artifact_view view;
        memset(&view, 0, sizeof(view));
        view.struct_size = sizeof(view);
        status = ql_artifact_get_view(parts[index], &view, error);
        if (status != QL_STATUS_OK) {
            ql_aig_blast_destroy(blast);
            return status;
        }
        if (strcmp(view.kind, QL_ARTIFACT_KIND_SMTLIB2) != 0) {
            ql_error_set(error, QL_STATUS_TYPE_MISMATCH,
                         "part %zu is not a quodlibet.smtlib2 artifact",
                         index);
            ql_aig_blast_destroy(blast);
            return QL_STATUS_TYPE_MISMATCH;
        }
        state.text = (const char *)view.data;
        state.size = view.size;
        state.cursor = 0u;
        while (!at_end(&state)) {
            status = parse_command(&state);
            if (status != QL_STATUS_OK) {
                ql_aig_blast_destroy(blast);
                return status;
            }
        }
    }
    *output = blast;
    ql_error_clear(error);
    return QL_STATUS_OK;
}

ql_status QL_CALL ql_aig_blast_get_view(const ql_aig_blast *blast,
                                        ql_aig_blast_view_v1 *view,
                                        ql_error *error) {
    if (blast == NULL || view == NULL) {
        ql_error_set(error, QL_STATUS_INVALID_ARGUMENT,
                     "blast result and view are required");
        return QL_STATUS_INVALID_ARGUMENT;
    }
    if (view->struct_size != 0u && view->struct_size < sizeof(*view)) {
        ql_error_set(error, QL_STATUS_ABI_MISMATCH,
                     "blast view structure is too small");
        return QL_STATUS_ABI_MISMATCH;
    }
    memset(view, 0, sizeof(*view));
    view->struct_size = sizeof(*view);
    view->schema_version = QL_AIG_SCHEMA_VERSION;
    view->root = blast->root;
    view->declared_count = (uint64_t)blast->declared_count;
    view->defined_count = (uint64_t)blast->defined_count;
    view->assertion_count = (uint64_t)blast->assertion_count;
    ql_error_clear(error);
    return QL_STATUS_OK;
}

size_t QL_CALL ql_aig_blast_symbol_count(const ql_aig_blast *blast) {
    return blast == NULL ? 0u : blast->declared_count;
}

static void fill_symbol_view(const blast_symbol *symbol,
                             ql_aig_blast_symbol_v1 *output) {
    memset(output, 0, sizeof(*output));
    output->struct_size = sizeof(*output);
    output->name = symbol->name;
    output->name_size = symbol->name_size;
    output->is_bool = symbol->value.is_bool;
    output->bit_width = symbol->value.width;
    output->first_input = symbol->first_input;
}

ql_status QL_CALL ql_aig_blast_symbol_at(const ql_aig_blast *blast,
                                         size_t index,
                                         ql_aig_blast_symbol_v1 *output,
                                         ql_error *error) {
    size_t seen = 0u;
    size_t position;

    if (blast == NULL || output == NULL) {
        ql_error_set(error, QL_STATUS_INVALID_ARGUMENT,
                     "blast result and output are required");
        return QL_STATUS_INVALID_ARGUMENT;
    }
    for (position = 0u; position < blast->symbol_count; ++position) {
        if (blast->symbols[position].is_declared == 0u) {
            continue;
        }
        if (seen == index) {
            fill_symbol_view(&blast->symbols[position], output);
            ql_error_clear(error);
            return QL_STATUS_OK;
        }
        ++seen;
    }
    ql_error_set(error, QL_STATUS_NOT_FOUND,
                 "the query declares no constant at index %zu", index);
    return QL_STATUS_NOT_FOUND;
}

ql_status QL_CALL ql_aig_blast_symbol_by_name(const ql_aig_blast *blast,
                                              const char *name,
                                              ql_aig_blast_symbol_v1 *output,
                                              ql_error *error) {
    const blast_symbol *symbol;

    if (blast == NULL || name == NULL || output == NULL) {
        ql_error_set(error, QL_STATUS_INVALID_ARGUMENT,
                     "blast result, name, and output are required");
        return QL_STATUS_INVALID_ARGUMENT;
    }
    symbol = symbol_find(blast, name, strlen(name));
    if (symbol == NULL || symbol->is_declared == 0u) {
        ql_error_set(error, QL_STATUS_NOT_FOUND,
                     "the query declares no constant named '%s'", name);
        return QL_STATUS_NOT_FOUND;
    }
    fill_symbol_view(symbol, output);
    ql_error_clear(error);
    return QL_STATUS_OK;
}

/* --- SAT assignment back to a solver model -------------------------------- */

typedef struct model_text {
    ql_allocator allocator;
    char *data;
    size_t size;
    size_t capacity;
} model_text;

static int model_text_reserve(model_text *text, size_t extra) {
    size_t needed = text->size + extra + 1u;
    size_t capacity;
    char *grown;

    if (needed <= text->capacity) {
        return 1;
    }
    capacity = text->capacity == 0u ? 256u : text->capacity;
    while (capacity < needed) {
        if (capacity > (SIZE_MAX / 2u)) {
            return 0;
        }
        capacity *= 2u;
    }
    grown = text->allocator.reallocate(text->allocator.user_data, text->data,
                                       capacity);
    if (grown == NULL) {
        return 0;
    }
    text->data = grown;
    text->capacity = capacity;
    return 1;
}

static int model_text_append(model_text *text, const char *bytes,
                             size_t size) {
    if (!model_text_reserve(text, size)) {
        return 0;
    }
    memcpy(text->data + text->size, bytes, size);
    text->size += size;
    text->data[text->size] = '\0';
    return 1;
}

static int model_text_append_cstr(model_text *text, const char *bytes) {
    return model_text_append(text, bytes, strlen(bytes));
}

ql_status QL_CALL ql_aig_blast_model_artifact_create(
    const ql_allocator *allocator, const ql_aig_blast *blast,
    const ql_aig_cnf *cnf, const int32_t *assignment, size_t assignment_count,
    ql_artifact **output, ql_error *error) {
    const ql_allocator *selected = select_allocator(allocator);
    ql_aig_cnf_view_v1 cnf_view;
    model_text text;
    uint8_t *values = NULL;
    size_t index;
    size_t symbol_count;
    ql_status status;

    if (blast == NULL || cnf == NULL || output == NULL ||
        (assignment_count != 0u && assignment == NULL)) {
        ql_error_set(error, QL_STATUS_INVALID_ARGUMENT,
                     "blast result, formula, assignment, and output are required");
        return QL_STATUS_INVALID_ARGUMENT;
    }
    *output = NULL;
    memset(&cnf_view, 0, sizeof(cnf_view));
    cnf_view.struct_size = sizeof(cnf_view);
    status = ql_aig_cnf_get_view(cnf, &cnf_view, error);
    if (status != QL_STATUS_OK) {
        return status;
    }
    memset(&text, 0, sizeof(text));
    text.allocator = *selected;

    /* One byte per variable: 0 unassigned or false, 1 true. A variable the
       solver did not mention is left false, which the replay then confirms or
       refutes concretely. */
    if (cnf_view.variable_count != 0u) {
        values = selected->allocate(selected->user_data,
                                    (size_t)cnf_view.variable_count + 1u);
        if (values == NULL) {
            ql_error_set(error, QL_STATUS_OUT_OF_MEMORY, NULL);
            return QL_STATUS_OUT_OF_MEMORY;
        }
        memset(values, 0, (size_t)cnf_view.variable_count + 1u);
    }
    for (index = 0u; index < assignment_count; ++index) {
        const int32_t literal = assignment[index];
        uint64_t variable;
        if (literal == 0) {
            continue;
        }
        variable = literal < 0 ? (uint64_t)(-(int64_t)literal)
                               : (uint64_t)literal;
        if (variable > cnf_view.variable_count) {
            ql_error_set(error, QL_STATUS_PARSE_ERROR,
                         "the assignment mentions variable %llu but the formula has %llu",
                         (unsigned long long)variable,
                         (unsigned long long)cnf_view.variable_count);
            status = QL_STATUS_PARSE_ERROR;
            goto cleanup;
        }
        values[variable] = literal > 0 ? 1u : 0u;
    }

    if (!model_text_append_cstr(&text, "(")) {
        ql_error_set(error, QL_STATUS_OUT_OF_MEMORY, NULL);
        status = QL_STATUS_OUT_OF_MEMORY;
        goto cleanup;
    }
    symbol_count = ql_aig_blast_symbol_count(blast);
    for (index = 0u; index < symbol_count; ++index) {
        ql_aig_blast_symbol_v1 symbol;
        char sort[48];
        uint32_t bit;
        int written;

        memset(&symbol, 0, sizeof(symbol));
        symbol.struct_size = sizeof(symbol);
        status = ql_aig_blast_symbol_at(blast, index, &symbol, error);
        if (status != QL_STATUS_OK) {
            goto cleanup;
        }
        if (!model_text_append_cstr(&text, "(define-fun ") ||
            !model_text_append(&text, symbol.name, symbol.name_size)) {
            status = QL_STATUS_OUT_OF_MEMORY;
            ql_error_set(error, status, NULL);
            goto cleanup;
        }
        if (symbol.is_bool != 0u) {
            const uint32_t variable =
                ql_aig_cnf_input_variable(cnf, symbol.first_input);
            const int set = variable != 0u && values != NULL &&
                            values[variable] != 0u;
            if (!model_text_append_cstr(&text, " () Bool ") ||
                !model_text_append_cstr(&text, set ? "true" : "false") ||
                !model_text_append_cstr(&text, ")")) {
                status = QL_STATUS_OUT_OF_MEMORY;
                ql_error_set(error, status, NULL);
                goto cleanup;
            }
            continue;
        }
        written = snprintf(sort, sizeof(sort), " () (_ BitVec %u) #b",
                           (unsigned)symbol.bit_width);
        if (written < 0 || (size_t)written >= sizeof(sort)) {
            ql_error_set(error, QL_STATUS_INTERNAL_ERROR,
                         "could not format a bit-vector sort");
            status = QL_STATUS_INTERNAL_ERROR;
            goto cleanup;
        }
        if (!model_text_append(&text, sort, (size_t)written)) {
            status = QL_STATUS_OUT_OF_MEMORY;
            ql_error_set(error, status, NULL);
            goto cleanup;
        }
        /* SMT-LIB binary literals run most significant bit first. */
        for (bit = symbol.bit_width; bit != 0u; --bit) {
            const uint32_t variable = ql_aig_cnf_input_variable(
                cnf, symbol.first_input + bit - 1u);
            const int set = variable != 0u && values != NULL &&
                            values[variable] != 0u;
            if (!model_text_append_cstr(&text, set ? "1" : "0")) {
                status = QL_STATUS_OUT_OF_MEMORY;
                ql_error_set(error, status, NULL);
                goto cleanup;
            }
        }
        if (!model_text_append_cstr(&text, ")")) {
            status = QL_STATUS_OUT_OF_MEMORY;
            ql_error_set(error, status, NULL);
            goto cleanup;
        }
    }
    if (!model_text_append_cstr(&text, ")")) {
        status = QL_STATUS_OUT_OF_MEMORY;
        ql_error_set(error, status, NULL);
        goto cleanup;
    }
    status = ql_artifact_create(selected, QL_ARTIFACT_KIND_SOLVER_MODEL,
                                QL_SOLVER_ARTIFACT_SCHEMA_VERSION, text.data,
                                text.size, output, error);

cleanup:
    selected->deallocate(selected->user_data, values);
    selected->deallocate(selected->user_data, text.data);
    return status;
}

/* ==========================================================================
 * prove.aig-sat
 *
 * The point of this method is one field: checked_proof. Everything below it
 * exists to make that field mean what it says.
 *
 * prove.smt-product answers the same question and, under an explicit
 * trusted-backend policy, believes Bitwuzla when it says UNSAT. This method
 * believes nobody. It blasts the same query bytes to CNF, runs CaDiCaL, and
 * for an UNSAT it takes the LRAT certificate to an independent checker over
 * the original CNF. Only if that checker verifies does anything get promoted,
 * and even then only after the comparison domain is shown to be inhabited by
 * an assignment this code evaluates itself, and after the problem's proof
 * binding holds. The trusted computing base is one vendored checker source
 * file plus this file's own arithmetic; the solver is not in it.
 *
 * The symmetric rule applies to SAT: a satisfying assignment is a claim, not a
 * counterexample, until the concrete replay reproduces the violation through
 * exactly the decoder and evaluator the SMT path uses.
 * ========================================================================== */

#define QL_AIG_SAT_CATEGORY "proof.aig-sat"

#ifndef QL_CADICAL_EXECUTABLE
#  define QL_CADICAL_EXECUTABLE ""
#endif
#ifndef QL_LRAT_CHECK_EXECUTABLE
#  define QL_LRAT_CHECK_EXECUTABLE ""
#endif

/* CaDiCaL's exit codes. Anything else is "it did not decide", which is an
   UNKNOWN and never a verdict. */
#define QL_AIG_SAT_EXIT_SATISFIABLE 10
#define QL_AIG_SAT_EXIT_UNSATISFIABLE 20

/* A witness is small; a proof is not. The proof never passes through this
   process's memory except to be hashed. */
#define QL_AIG_SAT_MAX_SOLVER_STDOUT ((size_t)(64u * 1024u * 1024u))
#define QL_AIG_SAT_MAX_CHECKER_STDOUT ((size_t)(4u * 1024u * 1024u))
#define QL_AIG_SAT_MAX_STDERR ((size_t)(1u * 1024u * 1024u))

static const char QL_AIG_SAT_CNF_NAME[] = "query.cnf";
static const char QL_AIG_SAT_PROOF_NAME[] = "query.lrat";

typedef struct aig_sat_instance {
    ql_allocator allocator;
    uint64_t timeout_ms;
    uint64_t memory_limit_mb;
    /* Null selects the binary this build vendored. */
    char *solver_executable;
    char *checker_executable;
} aig_sat_instance;

typedef struct aig_sat_decision {
    ql_verdict verdict;
    ql_evidence_class evidence_class;
    uint32_t checked_proof;
    uint32_t replay_confirmed;
    ql_aig_sat_answer violation_answer;
    ql_aig_sat_answer domain_answer;
    uint64_t cnf_variable_count;
    uint64_t cnf_clause_count;
    ql_digest solver_binary_digest;
    ql_digest checker_binary_digest;
    ql_digest cnf_digest;
    ql_digest proof_digest;
    char diagnostic[QL_ERROR_MESSAGE_CAPACITY];
} aig_sat_decision;

/* The two private executables and the directory their files live in. Built
   once per run and torn down with it: a binary swapped underneath a judgement
   cannot change what answered, and nothing survives to be read by the next
   one. */
typedef struct aig_sat_tools {
    ql_process_snapshot solver;
    ql_process_snapshot checker;
    ql_process_scratch scratch;
    uint32_t ready;
} aig_sat_tools;

/* One blasted query: the circuit, the symbol table a model decodes through,
   the CNF, and the DIMACS bytes handed to the solver. */
typedef struct aig_sat_query {
    ql_aig *aig;
    ql_aig_blast *blast;
    ql_aig_cnf *cnf;
    ql_artifact *dimacs;
    ql_aig_view_v1 aig_view;
    ql_aig_blast_view_v1 blast_view;
    ql_aig_cnf_view_v1 cnf_view;
} aig_sat_query;

static void *aig_sat_json_allocate(void *context, size_t size) {
    ql_allocator *allocator = (ql_allocator *)context;
    return allocator->allocate(allocator->user_data, size);
}

static void *aig_sat_json_reallocate(void *context, void *pointer,
                                     size_t old_size, size_t size) {
    ql_allocator *allocator = (ql_allocator *)context;
    (void)old_size;
    return allocator->reallocate(allocator->user_data, pointer, size);
}

static void aig_sat_json_deallocate(void *context, void *pointer) {
    ql_allocator *allocator = (ql_allocator *)context;
    allocator->deallocate(allocator->user_data, pointer);
}

static yyjson_alc aig_sat_json_allocator(ql_allocator *allocator) {
    yyjson_alc result;
    result.malloc = aig_sat_json_allocate;
    result.realloc = aig_sat_json_reallocate;
    result.free = aig_sat_json_deallocate;
    result.ctx = allocator;
    return result;
}

static void aig_sat_set_diagnostic(aig_sat_decision *decision,
                                   const char *message) {
    size_t length = strlen(message);
    if (length >= sizeof(decision->diagnostic)) {
        length = sizeof(decision->diagnostic) - 1u;
    }
    memcpy(decision->diagnostic, message, length);
    decision->diagnostic[length] = '\0';
}

static const char *aig_sat_answer_string(ql_aig_sat_answer answer) {
    switch (answer) {
    case QL_AIG_SAT_ANSWER_SAT:
        return "sat";
    case QL_AIG_SAT_ANSWER_UNSAT:
        return "unsat";
    case QL_AIG_SAT_ANSWER_UNKNOWN:
        return "unknown";
    case QL_AIG_SAT_ANSWER_TRIVIALLY_TRUE:
        return "trivially-true";
    case QL_AIG_SAT_ANSWER_TRIVIALLY_FALSE:
        return "trivially-false";
    default:
        return "not-queried";
    }
}

static ql_aig_sat_answer aig_sat_answer_parse(const char *text) {
    if (text == NULL) {
        return QL_AIG_SAT_ANSWER_NOT_QUERIED;
    }
    if (strcmp(text, "sat") == 0) {
        return QL_AIG_SAT_ANSWER_SAT;
    }
    if (strcmp(text, "unsat") == 0) {
        return QL_AIG_SAT_ANSWER_UNSAT;
    }
    if (strcmp(text, "unknown") == 0) {
        return QL_AIG_SAT_ANSWER_UNKNOWN;
    }
    if (strcmp(text, "trivially-true") == 0) {
        return QL_AIG_SAT_ANSWER_TRIVIALLY_TRUE;
    }
    if (strcmp(text, "trivially-false") == 0) {
        return QL_AIG_SAT_ANSWER_TRIVIALLY_FALSE;
    }
    return QL_AIG_SAT_ANSWER_NOT_QUERIED;
}

static ql_verdict aig_sat_proved_verdict_for(ql_relation relation) {
    switch (relation) {
    case QL_RELATION_LEFT_REFINES_RIGHT:
        return QL_VERDICT_PROVED_LEFT_REFINES_RIGHT;
    case QL_RELATION_RIGHT_REFINES_LEFT:
        return QL_VERDICT_PROVED_RIGHT_REFINES_LEFT;
    default:
        return QL_VERDICT_PROVED_EQUIVALENT;
    }
}

static const char *aig_sat_solver_path(const aig_sat_instance *instance) {
    return instance->solver_executable != NULL ? instance->solver_executable
                                               : QL_CADICAL_EXECUTABLE;
}

static const char *aig_sat_checker_path(const aig_sat_instance *instance) {
    return instance->checker_executable != NULL ? instance->checker_executable
                                                : QL_LRAT_CHECK_EXECUTABLE;
}

uint32_t QL_CALL ql_aig_sat_available(void) {
    return QL_CADICAL_EXECUTABLE[0] != '\0' &&
                   QL_LRAT_CHECK_EXECUTABLE[0] != '\0'
               ? 1u
               : 0u;
}

/* --- Options -------------------------------------------------------------- */

static int aig_sat_path_is_absolute(const char *path) {
    if (path == NULL || path[0] == '\0') {
        return 0;
    }
    if (path[0] == '/' || (path[0] == '\\' && path[1] == '\\')) {
        return 1;
    }
    return ((path[0] >= 'a' && path[0] <= 'z') ||
            (path[0] >= 'A' && path[0] <= 'Z')) &&
           path[1] == ':' && (path[2] == '/' || path[2] == '\\');
}

static ql_status aig_sat_copy_path(const ql_allocator *allocator,
                                   const char *text, char **output,
                                   const char *option, ql_error *error) {
    size_t size;
    char *copy;

    if (text == NULL || !aig_sat_path_is_absolute(text)) {
        ql_error_set(error, QL_STATUS_INVALID_ARGUMENT,
                     "%s must be an absolute path", option);
        return QL_STATUS_INVALID_ARGUMENT;
    }
    size = strlen(text);
    copy = (char *)allocator->allocate(allocator->user_data, size + 1u);
    if (copy == NULL) {
        ql_error_set(error, QL_STATUS_OUT_OF_MEMORY, NULL);
        return QL_STATUS_OUT_OF_MEMORY;
    }
    memcpy(copy, text, size + 1u);
    allocator->deallocate(allocator->user_data, *output);
    *output = copy;
    return QL_STATUS_OK;
}

static ql_status aig_sat_parse_options(aig_sat_instance *instance,
                                       const char *options_json,
                                       ql_error *error) {
    static const char *const known[] = {"timeout_ms", "memory_limit_mb",
                                        "solver_executable",
                                        "checker_executable"};
    yyjson_alc json_allocator = aig_sat_json_allocator(&instance->allocator);
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
                     QL_AIG_SAT_METHOD_NAME, read_error.pos,
                     read_error.msg != NULL ? read_error.msg : "parse error");
        return QL_STATUS_PARSE_ERROR;
    }
    root = yyjson_doc_get_root(document);
    if (!yyjson_is_obj(root)) {
        ql_error_set(error, QL_STATUS_PARSE_ERROR,
                     "%s options must be a JSON object",
                     QL_AIG_SAT_METHOD_NAME);
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
                         "%s does not accept the option provided",
                         QL_AIG_SAT_METHOD_NAME);
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
    value = yyjson_obj_get(root, "memory_limit_mb");
    if (value != NULL) {
        if (!yyjson_is_uint(value)) {
            ql_error_set(error, QL_STATUS_INVALID_ARGUMENT,
                         "memory_limit_mb must be a non-negative integer");
            status = QL_STATUS_INVALID_ARGUMENT;
            goto cleanup;
        }
        instance->memory_limit_mb = yyjson_get_uint(value);
    }
    value = yyjson_obj_get(root, "solver_executable");
    if (value != NULL) {
        status = aig_sat_copy_path(&instance->allocator,
                                   yyjson_get_str(value),
                                   &instance->solver_executable,
                                   "solver_executable", error);
        if (status != QL_STATUS_OK) {
            goto cleanup;
        }
    }
    value = yyjson_obj_get(root, "checker_executable");
    if (value != NULL) {
        status = aig_sat_copy_path(&instance->allocator,
                                   yyjson_get_str(value),
                                   &instance->checker_executable,
                                   "checker_executable", error);
        if (status != QL_STATUS_OK) {
            goto cleanup;
        }
    }
    ql_error_clear(error);

cleanup:
    yyjson_doc_free(document);
    return status;
}

/* --- The private installation --------------------------------------------- */

static void aig_sat_tools_close(aig_sat_tools *tools) {
    ql_process_scratch_dispose(&tools->scratch);
    ql_process_snapshot_dispose(&tools->checker);
    ql_process_snapshot_dispose(&tools->solver);
    memset(tools, 0, sizeof(*tools));
}

/* Both tools are copied somewhere private and hashed before either runs, and
   the digests recorded here are of the copies. A path names a file; a digest
   names bytes, and only the bytes answered anything. */
static ql_status aig_sat_tools_open(const aig_sat_instance *instance,
                                    aig_sat_tools *tools, ql_error *error) {
#if defined(_WIN32)
    static const char solver_name[] = "cadical.exe";
    static const char checker_name[] = "lrat-check.exe";
#else
    static const char solver_name[] = "cadical";
    static const char checker_name[] = "lrat-check";
#endif
    const char *solver_path = aig_sat_solver_path(instance);
    const char *checker_path = aig_sat_checker_path(instance);
    ql_status status;

    memset(tools, 0, sizeof(*tools));
    if (solver_path[0] == '\0' || checker_path[0] == '\0') {
        ql_error_set(error, QL_STATUS_NOT_FOUND,
                     "this build vendored no SAT backend or no proof checker");
        return QL_STATUS_NOT_FOUND;
    }
    status = ql_process_snapshot_create(&instance->allocator, solver_path,
                                        solver_name, &tools->solver, error);
    if (status == QL_STATUS_OK) {
        status = ql_process_snapshot_create(&instance->allocator,
                                            checker_path, checker_name,
                                            &tools->checker, error);
    }
    if (status == QL_STATUS_OK) {
        status = ql_process_scratch_create(&instance->allocator,
                                           "quodlibet-aig-sat",
                                           &tools->scratch, error);
    }
    if (status != QL_STATUS_OK) {
        aig_sat_tools_close(tools);
        return status;
    }
    tools->ready = 1u;
    return QL_STATUS_OK;
}

static void aig_sat_limits_init(const aig_sat_instance *instance,
                                const ql_run_context_v1 *context,
                                size_t stdout_limit,
                                ql_process_limits_v1 *limits) {
    ql_process_limits_init(limits);
    limits->timeout_ms = instance->timeout_ms;
    limits->memory_limit_mb = instance->memory_limit_mb;
    limits->stdout_limit_bytes = stdout_limit;
    limits->stderr_limit_bytes = QL_AIG_SAT_MAX_STDERR;
    if (context != NULL && context->is_cancelled != NULL) {
        limits->cancel_state = context->cancel_state;
        limits->is_cancelled = context->is_cancelled;
    }
}

/* --- Blasting ------------------------------------------------------------- */

static void aig_sat_query_dispose(aig_sat_query *query) {
    ql_artifact_release(query->dimacs);
    ql_aig_cnf_destroy(query->cnf);
    ql_aig_blast_destroy(query->blast);
    ql_aig_destroy(query->aig);
    memset(query, 0, sizeof(*query));
}

/* Blasts prefix + terminal into a circuit and a CNF. A grammar refusal comes
   back as QL_STATUS_TYPE_MISMATCH and is a boundary of this method, never a
   statement about the two functions. */
static ql_status aig_sat_query_build(const ql_allocator *allocator,
                                     const ql_artifact *prefix,
                                     const ql_artifact *terminal,
                                     aig_sat_query *query, ql_error *error) {
    const ql_artifact *parts[2];
    ql_status status;

    memset(query, 0, sizeof(*query));
    parts[0] = prefix;
    parts[1] = terminal;
    status = ql_aig_create(allocator, &query->aig, error);
    if (status != QL_STATUS_OK) {
        return status;
    }
    status = ql_aig_blast_smt2(query->aig, parts, 2u, &query->blast, error);
    if (status != QL_STATUS_OK) {
        aig_sat_query_dispose(query);
        return status;
    }
    ql_aig_view_init(&query->aig_view);
    status = ql_aig_get_view(query->aig, &query->aig_view, error);
    if (status == QL_STATUS_OK) {
        memset(&query->blast_view, 0, sizeof(query->blast_view));
        query->blast_view.struct_size = sizeof(query->blast_view);
        status = ql_aig_blast_get_view(query->blast, &query->blast_view,
                                       error);
    }
    if (status == QL_STATUS_OK) {
        status = ql_aig_cnf_create(allocator, query->aig,
                                   query->blast_view.root, &query->cnf,
                                   error);
    }
    if (status == QL_STATUS_OK) {
        memset(&query->cnf_view, 0, sizeof(query->cnf_view));
        query->cnf_view.struct_size = sizeof(query->cnf_view);
        status = ql_aig_cnf_get_view(query->cnf, &query->cnf_view, error);
    }
    if (status == QL_STATUS_OK) {
        status = ql_aig_cnf_artifact_create(allocator, query->cnf,
                                            &query->dimacs, error);
    }
    if (status != QL_STATUS_OK) {
        aig_sat_query_dispose(query);
        return status;
    }
    return QL_STATUS_OK;
}

/* --- Reading what the solver said ----------------------------------------- */

/* The DIMACS literals on the solver's `v` lines. Nothing else on stdout is
   read: a line the format does not define is not evidence. */
static ql_status aig_sat_parse_assignment(const ql_allocator *allocator,
                                          const char *text, size_t size,
                                          int32_t **output, size_t *count,
                                          ql_error *error) {
    int32_t *values = NULL;
    size_t used = 0u;
    size_t capacity = 0u;
    size_t offset = 0u;

    *output = NULL;
    *count = 0u;
    while (offset < size) {
        size_t line_end = offset;
        size_t cursor;

        while (line_end < size && text[line_end] != '\n') {
            ++line_end;
        }
        cursor = offset;
        if (cursor < line_end && text[cursor] == 'v' &&
            (cursor + 1u == line_end || text[cursor + 1u] == ' ' ||
             text[cursor + 1u] == '\t' || text[cursor + 1u] == '\r')) {
            ++cursor;
            while (cursor < line_end) {
                char token[32];
                size_t token_size = 0u;
                long parsed;
                char *end = NULL;

                while (cursor < line_end &&
                       (text[cursor] == ' ' || text[cursor] == '\t' ||
                        text[cursor] == '\r')) {
                    ++cursor;
                }
                if (cursor >= line_end) {
                    break;
                }
                while (cursor < line_end && text[cursor] != ' ' &&
                       text[cursor] != '\t' && text[cursor] != '\r') {
                    if (token_size + 1u >= sizeof(token)) {
                        allocator->deallocate(allocator->user_data, values);
                        ql_error_set(error, QL_STATUS_PARSE_ERROR,
                                     "the solver witness contains a token no DIMACS literal can be");
                        return QL_STATUS_PARSE_ERROR;
                    }
                    token[token_size++] = text[cursor++];
                }
                token[token_size] = '\0';
                parsed = strtol(token, &end, 10);
                if (end == token || *end != '\0' || parsed > INT32_MAX ||
                    parsed < -(long)INT32_MAX) {
                    allocator->deallocate(allocator->user_data, values);
                    ql_error_set(error, QL_STATUS_PARSE_ERROR,
                                 "the solver witness contains a token that is not a DIMACS literal");
                    return QL_STATUS_PARSE_ERROR;
                }
                if (parsed == 0) {
                    continue;
                }
                if (used == capacity) {
                    const size_t next = capacity == 0u ? 64u : capacity * 2u;
                    int32_t *grown = (int32_t *)allocator->reallocate(
                        allocator->user_data, values, next * sizeof(int32_t));
                    if (grown == NULL) {
                        allocator->deallocate(allocator->user_data, values);
                        ql_error_set(error, QL_STATUS_OUT_OF_MEMORY, NULL);
                        return QL_STATUS_OUT_OF_MEMORY;
                    }
                    values = grown;
                    capacity = next;
                }
                values[used++] = (int32_t)parsed;
            }
        }
        offset = line_end < size ? line_end + 1u : line_end;
    }
    *output = values;
    *count = used;
    ql_error_clear(error);
    return QL_STATUS_OK;
}

/* Evaluates the blasted root on the assignment the solver returned.

   This is what keeps a SAT answer from being taken on trust where no replay
   follows it: the domain query has no concrete semantics to re-run, so its
   satisfiability is confirmed against the circuit by this process rather than
   believed because a solver printed a word. An input the cone never reached
   has no variable and is read as zero, which is sound because the root's value
   does not depend on it. */
static ql_status aig_sat_assignment_satisfies_root(
    const ql_allocator *allocator, const aig_sat_query *query,
    const int32_t *assignment, size_t assignment_count, uint32_t *satisfied,
    ql_error *error) {
    const uint64_t variable_count = query->cnf_view.variable_count;
    const uint64_t input_count = query->aig_view.input_count;
    uint8_t *variable_values = NULL;
    uint8_t *input_values = NULL;
    uint32_t value = 0u;
    uint64_t index;
    size_t entry;
    ql_status status;

    *satisfied = 0u;
    if (variable_count > SIZE_MAX - 1u || input_count > SIZE_MAX) {
        ql_error_set(error, QL_STATUS_METHOD_ERROR,
                     "the CNF is larger than this process can evaluate");
        return QL_STATUS_METHOD_ERROR;
    }
    variable_values = (uint8_t *)allocator->allocate(
        allocator->user_data, (size_t)variable_count + 1u);
    input_values = (uint8_t *)allocator->allocate(
        allocator->user_data, input_count == 0u ? 1u : (size_t)input_count);
    if (variable_values == NULL || input_values == NULL) {
        allocator->deallocate(allocator->user_data, variable_values);
        allocator->deallocate(allocator->user_data, input_values);
        ql_error_set(error, QL_STATUS_OUT_OF_MEMORY, NULL);
        return QL_STATUS_OUT_OF_MEMORY;
    }
    memset(variable_values, 0, (size_t)variable_count + 1u);
    memset(input_values, 0, input_count == 0u ? 1u : (size_t)input_count);
    for (entry = 0u; entry < assignment_count; ++entry) {
        const int32_t literal = assignment[entry];
        const uint64_t variable = literal < 0
                                      ? (uint64_t)(-(int64_t)literal)
                                      : (uint64_t)literal;
        if (variable == 0u || variable > variable_count) {
            allocator->deallocate(allocator->user_data, variable_values);
            allocator->deallocate(allocator->user_data, input_values);
            ql_error_set(error, QL_STATUS_PARSE_ERROR,
                         "the solver witness names a variable this CNF does not have");
            return QL_STATUS_PARSE_ERROR;
        }
        variable_values[variable] = literal > 0 ? 1u : 0u;
    }
    for (index = 0u; index < input_count; ++index) {
        const uint32_t variable =
            ql_aig_cnf_input_variable(query->cnf, (uint32_t)index);
        if (variable != 0u && (uint64_t)variable <= variable_count) {
            input_values[index] = variable_values[variable];
        }
    }
    status = ql_aig_evaluate(query->aig, input_values, (size_t)input_count,
                             query->blast_view.root, &value, error);
    allocator->deallocate(allocator->user_data, variable_values);
    allocator->deallocate(allocator->user_data, input_values);
    if (status != QL_STATUS_OK) {
        return status;
    }
    *satisfied = value != 0u ? 1u : 0u;
    return QL_STATUS_OK;
}

/* The checker's verdict is a whole line, never a substring. "NOT VERIFIED"
   contains "VERIFIED", and a substring search would read a refusal as an
   approval, which is the single worst mistake this file could make. */
static uint32_t aig_sat_checker_verified(const char *text, size_t size) {
    size_t offset = 0u;

    while (offset < size) {
        size_t line_end = offset;
        size_t begin;
        size_t end;

        while (line_end < size && text[line_end] != '\n') {
            ++line_end;
        }
        begin = offset;
        end = line_end;
        while (end > begin && (text[end - 1u] == '\r' ||
                               text[end - 1u] == ' ' ||
                               text[end - 1u] == '\t')) {
            --end;
        }
        /* The checker prefixes its output with DIMACS comment markers. */
        while (begin < end && (text[begin] == ' ' || text[begin] == '\t')) {
            ++begin;
        }
        if (begin < end && text[begin] == 'c' &&
            (begin + 1u == end || text[begin + 1u] == ' ' ||
             text[begin + 1u] == '\t')) {
            ++begin;
            while (begin < end && (text[begin] == ' ' ||
                                   text[begin] == '\t')) {
                ++begin;
            }
        }
        if (end - begin == 8u && memcmp(text + begin, "VERIFIED", 8u) == 0) {
            return 1u;
        }
        offset = line_end < size ? line_end + 1u : line_end;
    }
    return 0u;
}

/* --- Running the two tools ------------------------------------------------ */

typedef struct aig_sat_run {
    ql_aig_sat_answer answer;
    int32_t *assignment;
    size_t assignment_count;
} aig_sat_run;

static void aig_sat_run_dispose(const ql_allocator *allocator,
                                aig_sat_run *run) {
    allocator->deallocate(allocator->user_data, run->assignment);
    memset(run, 0, sizeof(*run));
}

/* Solves one CNF. `proof_name` is null when no certificate is wanted, which is
   the inhabitance query: a SAT answer there is confirmed by evaluating the
   circuit, and an UNSAT answer there promotes nothing. */
static ql_status aig_sat_solve(const aig_sat_instance *instance,
                               const aig_sat_tools *tools,
                               const ql_run_context_v1 *context,
                               const aig_sat_query *query,
                               const char *cnf_name, const char *proof_name,
                               aig_sat_run *run, ql_error *error) {
    char cnf_path[QL_PROCESS_SCRATCH_PATH_CAPACITY];
    char proof_path[QL_PROCESS_SCRATCH_PATH_CAPACITY];
    const char *arguments[5];
    size_t argument_count = 0u;
    ql_artifact_view dimacs_view;
    ql_process_limits_v1 limits;
    ql_process_result_v1 result;
    ql_status status;

    memset(run, 0, sizeof(*run));
    run->answer = QL_AIG_SAT_ANSWER_UNKNOWN;
    /* A root the encoding folded to a constant is an answer this process
       produced. It is reported as such and never dressed up as a solver
       result, because no certificate exists for it. */
    if (query->cnf_view.trivially_true != 0u) {
        run->answer = QL_AIG_SAT_ANSWER_TRIVIALLY_TRUE;
        ql_error_clear(error);
        return QL_STATUS_OK;
    }
    if (query->cnf_view.trivially_false != 0u) {
        run->answer = QL_AIG_SAT_ANSWER_TRIVIALLY_FALSE;
        ql_error_clear(error);
        return QL_STATUS_OK;
    }
    memset(&dimacs_view, 0, sizeof(dimacs_view));
    dimacs_view.struct_size = sizeof(dimacs_view);
    status = ql_artifact_get_view(query->dimacs, &dimacs_view, error);
    if (status != QL_STATUS_OK) {
        return status;
    }
    status = ql_process_scratch_write(&tools->scratch, cnf_name,
                                      dimacs_view.data, dimacs_view.size,
                                      error);
    if (status == QL_STATUS_OK) {
        status = ql_process_scratch_path(&tools->scratch, cnf_name, cnf_path,
                                         sizeof(cnf_path), error);
    }
    if (status == QL_STATUS_OK && proof_name != NULL) {
        status = ql_process_scratch_path(&tools->scratch, proof_name,
                                         proof_path, sizeof(proof_path),
                                         error);
    }
    if (status != QL_STATUS_OK) {
        return status;
    }
    arguments[argument_count++] = "-q";
    if (proof_name != NULL) {
        arguments[argument_count++] = "--lrat";
        /* The certificate has to be readable by a checker whose whole value is
           that it is small enough to audit. A binary proof is not that. */
        arguments[argument_count++] = "--no-binary";
    }
    arguments[argument_count++] = cnf_path;
    if (proof_name != NULL) {
        arguments[argument_count++] = proof_path;
    }
    aig_sat_limits_init(instance, context, QL_AIG_SAT_MAX_SOLVER_STDOUT,
                        &limits);
    ql_process_result_init(&result);
    status = ql_process_run(&instance->allocator,
                            tools->solver.executable_path, arguments,
                            argument_count, NULL, 0u, &limits, &result,
                            error);
    if (status != QL_STATUS_OK) {
        return status;
    }
    if (result.cancelled != 0u || result.timed_out != 0u) {
        ql_process_result_dispose(&instance->allocator, &result);
        run->answer = QL_AIG_SAT_ANSWER_UNKNOWN;
        ql_error_clear(error);
        return QL_STATUS_OK;
    }
    if (result.exit_status == QL_AIG_SAT_EXIT_SATISFIABLE) {
        status = aig_sat_parse_assignment(&instance->allocator,
                                          result.stdout_text,
                                          result.stdout_size,
                                          &run->assignment,
                                          &run->assignment_count, error);
        if (status == QL_STATUS_OK) {
            run->answer = QL_AIG_SAT_ANSWER_SAT;
        }
    } else if (result.exit_status == QL_AIG_SAT_EXIT_UNSATISFIABLE) {
        run->answer = QL_AIG_SAT_ANSWER_UNSAT;
    } else {
        run->answer = QL_AIG_SAT_ANSWER_UNKNOWN;
    }
    ql_process_result_dispose(&instance->allocator, &result);
    if (status == QL_STATUS_OK) {
        ql_error_clear(error);
    }
    return status;
}

/* Takes the certificate to the checker over the original CNF. This is the one
   step the whole method exists for, so it is deliberately unforgiving: the
   checker must exit zero and must print its approval as a whole line. */
static ql_status aig_sat_check_certificate(const aig_sat_instance *instance,
                                           const aig_sat_tools *tools,
                                           const ql_run_context_v1 *context,
                                           const char *cnf_name,
                                           const char *proof_name,
                                           uint32_t *verified,
                                           ql_error *error) {
    char cnf_path[QL_PROCESS_SCRATCH_PATH_CAPACITY];
    char proof_path[QL_PROCESS_SCRATCH_PATH_CAPACITY];
    const char *arguments[2];
    ql_process_limits_v1 limits;
    ql_process_result_v1 result;
    ql_status status;

    *verified = 0u;
    status = ql_process_scratch_path(&tools->scratch, cnf_name, cnf_path,
                                     sizeof(cnf_path), error);
    if (status == QL_STATUS_OK) {
        status = ql_process_scratch_path(&tools->scratch, proof_name,
                                         proof_path, sizeof(proof_path),
                                         error);
    }
    if (status != QL_STATUS_OK) {
        return status;
    }
    arguments[0] = cnf_path;
    arguments[1] = proof_path;
    aig_sat_limits_init(instance, context, QL_AIG_SAT_MAX_CHECKER_STDOUT,
                        &limits);
    ql_process_result_init(&result);
    status = ql_process_run(&instance->allocator,
                            tools->checker.executable_path, arguments, 2u,
                            NULL, 0u, &limits, &result, error);
    if (status != QL_STATUS_OK) {
        return status;
    }
    if (result.cancelled == 0u && result.timed_out == 0u &&
        result.exit_status == 0 && result.term_signal == 0) {
        *verified = aig_sat_checker_verified(result.stdout_text,
                                             result.stdout_size);
    }
    ql_process_result_dispose(&instance->allocator, &result);
    ql_error_clear(error);
    return QL_STATUS_OK;
}

/* --- Deciding ------------------------------------------------------------- */

/* Turns a satisfying assignment into the same counterexample the SMT path
   would produce, or into nothing. The assignment becomes a solver-model
   artifact, the model goes through ql_replay_decode_model, and the decoded
   witness is executed concretely. Only a replay that reproduces the violation
   makes a counterexample. */
static ql_status aig_sat_confirm_witness(const ql_allocator *allocator,
                                         const ql_problem *problem,
                                         const ql_product_query *product,
                                         const aig_sat_query *query,
                                         const aig_sat_run *run,
                                         const ql_ir *left_ir,
                                         const ql_ir *right_ir,
                                         aig_sat_decision *decision,
                                         ql_artifact **counterexample,
                                         ql_error *error) {
    ql_artifact *model = NULL;
    ql_replay_witness *witness = NULL;
    ql_replay_result_v1 replay;
    ql_status status;

    status = ql_aig_blast_model_artifact_create(
        allocator, query->blast, query->cnf, run->assignment,
        run->assignment_count, &model, error);
    if (status != QL_STATUS_OK) {
        return status;
    }
    status = ql_replay_decode_model(allocator, product, model, &witness,
                                    error);
    if (status != QL_STATUS_OK) {
        QL_LOGE(QL_AIG_SAT_CATEGORY,
                "the SAT assignment could not be decoded into typed inputs: %s",
                error->message);
        aig_sat_set_diagnostic(
            decision,
            "the SAT assignment could not be decoded into typed inputs, so no counterexample is claimed");
        ql_artifact_release(model);
        ql_error_clear(error);
        return QL_STATUS_OK;
    }
    memset(&replay, 0, sizeof(replay));
    replay.struct_size = sizeof(replay);
    status = ql_replay_execute(allocator, problem, product, left_ir, right_ir,
                               witness, &replay, error);
    if (status != QL_STATUS_OK) {
        ql_replay_witness_destroy(witness);
        ql_artifact_release(model);
        return status;
    }
    if (replay.conclusive == 0u) {
        aig_sat_set_diagnostic(
            decision,
            "the decoded witness could not be replayed conclusively, so no counterexample is claimed");
    } else if (replay.violated == 0u) {
        /* The blaster and the concrete semantics disagree. That is a defect in
           this method, not a fact about the two functions, and burying it
           would be the exact failure this workstream exists to prevent. */
        QL_LOGE(QL_AIG_SAT_CATEGORY,
                "the SAT backend reported a satisfying assignment that concrete replay did not reproduce; the bit-blasting and the IR semantics disagree");
        aig_sat_set_diagnostic(
            decision,
            "concrete replay did not reproduce the violation; this indicates an encoding defect and is reported as UNKNOWN");
    } else {
        status = ql_replay_counterexample_artifact_create(
            allocator, product, witness, &replay, counterexample, error);
        if (status == QL_STATUS_OK) {
            decision->verdict = QL_VERDICT_COUNTEREXAMPLE;
            decision->evidence_class = QL_EVIDENCE_COUNTEREXAMPLE;
            decision->replay_confirmed = 1u;
            aig_sat_set_diagnostic(
                decision,
                "a SAT assignment decoded through the shared model decoder reproduced the violation under concrete replay");
        }
    }
    ql_replay_witness_destroy(witness);
    ql_artifact_release(model);
    return status;
}

/* Is the comparison domain inhabited? An UNSAT over an empty domain says
   nothing about the two functions, so this gate stands between a verified
   certificate and any promotion. The SAT answer here is confirmed by
   evaluating the circuit on the returned assignment rather than believed. */
static ql_status aig_sat_domain_inhabited(const ql_allocator *allocator,
                                          const aig_sat_instance *instance,
                                          const aig_sat_tools *tools,
                                          const ql_run_context_v1 *context,
                                          const ql_product_query *product,
                                          aig_sat_decision *decision,
                                          uint32_t *inhabited,
                                          ql_error *error) {
    aig_sat_query query;
    aig_sat_run run;
    ql_status status;

    *inhabited = 0u;
    status = aig_sat_query_build(allocator,
                                 ql_product_query_prefix_artifact(product),
                                 ql_product_query_domain_artifact(product),
                                 &query, error);
    if (status == QL_STATUS_TYPE_MISMATCH) {
        aig_sat_set_diagnostic(
            decision,
            "the comparison domain query is outside the grammar this blaster accepts, so no proof is claimed");
        ql_error_clear(error);
        return QL_STATUS_OK;
    }
    if (status != QL_STATUS_OK) {
        return status;
    }
    status = aig_sat_solve(instance, tools, context, &query, "domain.cnf",
                           NULL, &run, error);
    if (status != QL_STATUS_OK) {
        aig_sat_query_dispose(&query);
        return status;
    }
    decision->domain_answer = run.answer;
    switch (run.answer) {
    case QL_AIG_SAT_ANSWER_TRIVIALLY_TRUE:
        /* Every input is in the domain, by folding rather than by search. */
        *inhabited = 1u;
        break;
    case QL_AIG_SAT_ANSWER_SAT: {
        uint32_t satisfied = 0u;
        status = aig_sat_assignment_satisfies_root(
            allocator, &query, run.assignment, run.assignment_count,
            &satisfied, error);
        if (status == QL_STATUS_PARSE_ERROR) {
            aig_sat_set_diagnostic(
                decision,
                "the inhabitance witness could not be read, so the domain is not treated as inhabited");
            ql_error_clear(error);
            status = QL_STATUS_OK;
        } else if (status == QL_STATUS_OK && satisfied == 0u) {
            QL_LOGE(QL_AIG_SAT_CATEGORY,
                    "the SAT backend claimed the comparison domain is inhabited but its own witness does not satisfy the blasted domain circuit");
            aig_sat_set_diagnostic(
                decision,
                "the inhabitance witness does not satisfy the domain circuit; the domain is not treated as inhabited");
        } else if (status == QL_STATUS_OK) {
            *inhabited = 1u;
        }
        break;
    }
    case QL_AIG_SAT_ANSWER_UNSAT:
    case QL_AIG_SAT_ANSWER_TRIVIALLY_FALSE:
        aig_sat_set_diagnostic(
            decision,
            "the precondition and UB policy leave an empty comparison domain, so this UNSAT is vacuous");
        break;
    default:
        aig_sat_set_diagnostic(
            decision,
            "the SAT backend did not decide whether the comparison domain is inhabited");
        break;
    }
    aig_sat_run_dispose(allocator, &run);
    aig_sat_query_dispose(&query);
    return status;
}

static ql_status aig_sat_decide(const ql_allocator *allocator,
                                const aig_sat_instance *instance,
                                const ql_run_context_v1 *context,
                                const ql_problem *problem,
                                const ql_product_query *product,
                                const ql_product_query_view_v1 *query_view,
                                const ql_ir *left_ir, const ql_ir *right_ir,
                                aig_sat_decision *decision,
                                ql_artifact **counterexample,
                                ql_error *error) {
    aig_sat_tools tools;
    aig_sat_query query;
    aig_sat_run run;
    ql_artifact_view dimacs_view;
    uint32_t verified = 0u;
    uint32_t inhabited = 0u;
    ql_status status;

    *counterexample = NULL;
    memset(&query, 0, sizeof(query));
    memset(&run, 0, sizeof(run));
    status = aig_sat_tools_open(instance, &tools, error);
    if (status == QL_STATUS_NOT_FOUND) {
        aig_sat_set_diagnostic(
            decision,
            "this build vendored no SAT backend or no proof checker, so nothing was solved");
        ql_error_clear(error);
        return QL_STATUS_OK;
    }
    if (status != QL_STATUS_OK) {
        return status;
    }
    decision->solver_binary_digest = tools.solver.digest;
    decision->checker_binary_digest = tools.checker.digest;

    status = aig_sat_query_build(allocator,
                                 ql_product_query_prefix_artifact(product),
                                 ql_product_query_violation_artifact(product),
                                 &query, error);
    if (status == QL_STATUS_TYPE_MISMATCH) {
        aig_sat_set_diagnostic(decision, error->message);
        aig_sat_tools_close(&tools);
        ql_error_clear(error);
        return QL_STATUS_OK;
    }
    if (status == QL_STATUS_METHOD_ERROR) {
        /* An oversized circuit is a limit of this encoding, not a fact. */
        aig_sat_set_diagnostic(decision, error->message);
        aig_sat_tools_close(&tools);
        ql_error_clear(error);
        return QL_STATUS_OK;
    }
    if (status != QL_STATUS_OK) {
        aig_sat_tools_close(&tools);
        return status;
    }
    decision->cnf_variable_count = query.cnf_view.variable_count;
    decision->cnf_clause_count = query.cnf_view.clause_count;
    memset(&dimacs_view, 0, sizeof(dimacs_view));
    dimacs_view.struct_size = sizeof(dimacs_view);
    status = ql_artifact_get_view(query.dimacs, &dimacs_view, error);
    if (status == QL_STATUS_OK) {
        decision->cnf_digest = dimacs_view.digest;
        status = aig_sat_solve(instance, &tools, context, &query,
                               QL_AIG_SAT_CNF_NAME, QL_AIG_SAT_PROOF_NAME,
                               &run, error);
    }
    if (status != QL_STATUS_OK) {
        goto cleanup;
    }
    decision->violation_answer = run.answer;

    if (run.answer == QL_AIG_SAT_ANSWER_SAT ||
        run.answer == QL_AIG_SAT_ANSWER_TRIVIALLY_TRUE) {
        /* A folded-true root means every input violates, so the all-zero
           assignment is as good a witness as any; the replay decides. */
        status = aig_sat_confirm_witness(allocator, problem, product, &query,
                                         &run, left_ir, right_ir, decision,
                                         counterexample, error);
        goto cleanup;
    }
    if (run.answer == QL_AIG_SAT_ANSWER_TRIVIALLY_FALSE) {
        /* The encoding folded the miter to false. That is this process's own
           arithmetic and no certificate exists for it, so it is not promoted.
           Believing it would be believing exactly the thing this method was
           built to stop believing. */
        aig_sat_set_diagnostic(
            decision,
            "the encoding folded the miter to false without a solver, and an unchecked folding is not a proof");
        goto cleanup;
    }
    if (run.answer != QL_AIG_SAT_ANSWER_UNSAT) {
        aig_sat_set_diagnostic(
            decision, "the SAT backend did not decide the violation query");
        goto cleanup;
    }

    status = aig_sat_check_certificate(instance, &tools, context,
                                       QL_AIG_SAT_CNF_NAME,
                                       QL_AIG_SAT_PROOF_NAME, &verified,
                                       error);
    if (status != QL_STATUS_OK) {
        goto cleanup;
    }
    {
        char proof_path[QL_PROCESS_SCRATCH_PATH_CAPACITY];
        if (ql_process_scratch_path(&tools.scratch, QL_AIG_SAT_PROOF_NAME,
                                    proof_path, sizeof(proof_path),
                                    error) == QL_STATUS_OK) {
            /* The proof is a file, not an executable, but this is the one
               place that hashes a file under the module's open discipline. */
            (void)ql_process_executable_digest(proof_path,
                                               &decision->proof_digest,
                                               error);
        }
        ql_error_clear(error);
    }
    if (verified == 0u) {
        QL_LOGE(QL_AIG_SAT_CATEGORY,
                "the SAT backend reported UNSAT but its LRAT certificate did not verify against the original CNF");
        aig_sat_set_diagnostic(
            decision,
            "the UNSAT certificate did not verify against the original CNF, so nothing is promoted");
        goto cleanup;
    }
    status = aig_sat_domain_inhabited(allocator, instance, &tools, context,
                                      product, decision, &inhabited, error);
    if (status != QL_STATUS_OK || inhabited == 0u) {
        if (status == QL_STATUS_OK && decision->diagnostic[0] == '\0') {
            aig_sat_set_diagnostic(
                decision,
                "the comparison domain was not shown to be inhabited, so this UNSAT is not promoted");
        }
        goto cleanup;
    }
    status = ql_problem_require_proof_binding(problem, error);
    if (status != QL_STATUS_OK) {
        aig_sat_set_diagnostic(decision, error->message);
        status = QL_STATUS_OK;
        ql_error_clear(error);
        goto cleanup;
    }
    decision->verdict = aig_sat_proved_verdict_for(query_view->relation);
    decision->evidence_class = QL_EVIDENCE_PROOF;
    decision->checked_proof = 1u;
    aig_sat_set_diagnostic(
        decision,
        "the miter is unsatisfiable, its LRAT certificate verified against the original CNF, and the comparison domain is inhabited");

cleanup:
    aig_sat_run_dispose(allocator, &run);
    aig_sat_query_dispose(&query);
    aig_sat_tools_close(&tools);
    return status;
}

/* --- Outcome serialization ------------------------------------------------ */

static int aig_sat_add_digest(yyjson_mut_doc *document,
                              yyjson_mut_val *object, const char *key,
                              const ql_digest *digest) {
    char hex[QL_DIGEST_HEX_SIZE];
    ql_digest_hex(digest, hex);
    return yyjson_mut_obj_add_strcpy(document, object, key, hex);
}

/* The tool identities are in the key because a different solver or a different
   checker is a different search and a different amount of trust. */
static ql_status aig_sat_compute_cache_key(
    const ql_digest *problem_digest, const aig_sat_instance *instance,
    const ql_product_query_view_v1 *query,
    const aig_sat_decision *decision, ql_digest *cache_key,
    ql_error *error) {
    char identity[640];
    char prefix_hex[QL_DIGEST_HEX_SIZE];
    char violation_hex[QL_DIGEST_HEX_SIZE];
    char domain_hex[QL_DIGEST_HEX_SIZE];
    char solver_hex[QL_DIGEST_HEX_SIZE];
    char checker_hex[QL_DIGEST_HEX_SIZE];
    ql_cache_key_input_v1 input;
    int written;

    ql_digest_hex(&query->prefix_digest, prefix_hex);
    ql_digest_hex(&query->violation_digest, violation_hex);
    ql_digest_hex(&query->domain_digest, domain_hex);
    ql_digest_hex(&decision->solver_binary_digest, solver_hex);
    ql_digest_hex(&decision->checker_binary_digest, checker_hex);
    written = snprintf(
        identity, sizeof(identity),
        "{\"timeout_ms\":%llu,\"memory_limit_mb\":%llu,\"prefix\":\"%s\",\"violation\":\"%s\",\"domain\":\"%s\",\"solver\":\"%s\",\"checker\":\"%s\"}",
        (unsigned long long)instance->timeout_ms,
        (unsigned long long)instance->memory_limit_mb, prefix_hex,
        violation_hex, domain_hex, solver_hex, checker_hex);
    if (written < 0 || (size_t)written >= sizeof(identity)) {
        ql_error_set(error, QL_STATUS_INTERNAL_ERROR,
                     "could not format the cache identity");
        return QL_STATUS_INTERNAL_ERROR;
    }
    memset(&input, 0, sizeof(input));
    input.struct_size = sizeof(input);
    input.artifact_digest = *problem_digest;
    input.semantic_problem_digest = *problem_digest;
    input.method_name = QL_AIG_SAT_METHOD_NAME;
    input.method_version = QL_AIG_SAT_METHOD_VERSION;
    input.canonical_options = identity;
    input.canonical_options_size = (size_t)written;
    return ql_cache_key_compute(&input, cache_key, error);
}

static ql_status aig_sat_build_outcome(
    const ql_allocator *allocator, const aig_sat_instance *instance,
    const ql_problem_view_v2 *problem_view,
    const ql_product_query_view_v1 *query_view,
    const aig_sat_decision *decision, const ql_artifact *counterexample,
    ql_artifact **output, ql_error *error) {
    ql_allocator allocator_copy = *allocator;
    yyjson_alc json_allocator = aig_sat_json_allocator(&allocator_copy);
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
    status = aig_sat_compute_cache_key(&problem_view->artifact_digest,
                                       instance, query_view, decision,
                                       &cache_key, error);
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
                                 QL_AIG_SAT_OUTCOME_SCHEMA_VERSION) ||
        !yyjson_mut_obj_add_str(document, root, "method",
                                QL_AIG_SAT_METHOD_NAME) ||
        !yyjson_mut_obj_add_str(document, root, "method_version",
                                QL_AIG_SAT_METHOD_VERSION) ||
        !yyjson_mut_obj_add_str(document, root, "verdict",
                                ql_verdict_string(decision->verdict)) ||
        !yyjson_mut_obj_add_str(
            document, root, "evidence_class",
            ql_evidence_class_string(decision->evidence_class)) ||
        !aig_sat_add_digest(document, root, "problem_digest",
                            &problem_view->artifact_digest) ||
        !aig_sat_add_digest(document, root, "cache_key", &cache_key) ||
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
    /* The same three query digests the SMT path records. Two backends, one
       question: an envelope that did not say so would leave a reader to take
       it on faith. */
    if (!yyjson_mut_obj_add_str(document, search_object, "backend",
                                "cadical") ||
        !yyjson_mut_obj_add_str(document, search_object, "checker",
                                "lrat-check") ||
        !yyjson_mut_obj_add_str(document, search_object, "proof_format",
                                "lrat") ||
        !yyjson_mut_obj_add_str(
            document, search_object, "violation_answer",
            aig_sat_answer_string(decision->violation_answer)) ||
        !yyjson_mut_obj_add_str(
            document, search_object, "domain_answer",
            aig_sat_answer_string(decision->domain_answer)) ||
        !yyjson_mut_obj_add_uint(document, search_object,
                                 "cnf_variable_count",
                                 decision->cnf_variable_count) ||
        !yyjson_mut_obj_add_uint(document, search_object, "cnf_clause_count",
                                 decision->cnf_clause_count) ||
        !aig_sat_add_digest(document, search_object, "prefix_digest",
                            &query_view->prefix_digest) ||
        !aig_sat_add_digest(document, search_object, "violation_digest",
                            &query_view->violation_digest) ||
        !aig_sat_add_digest(document, search_object, "domain_digest",
                            &query_view->domain_digest) ||
        !aig_sat_add_digest(document, search_object, "solver_binary_digest",
                            &decision->solver_binary_digest) ||
        !aig_sat_add_digest(document, search_object, "checker_binary_digest",
                            &decision->checker_binary_digest) ||
        !aig_sat_add_digest(document, search_object, "cnf_digest",
                            &decision->cnf_digest) ||
        !aig_sat_add_digest(document, search_object, "proof_digest",
                            &decision->proof_digest) ||
        !yyjson_mut_obj_add_val(document, root, "search", search_object)) {
        status = QL_STATUS_OUT_OF_MEMORY;
        ql_error_set(error, status, NULL);
        goto cleanup;
    }
    if (!yyjson_mut_obj_add_bool(document, trust_object, "checked_proof",
                                 decision->checked_proof != 0u) ||
        !yyjson_mut_obj_add_bool(document, trust_object, "replay_confirmed",
                                 decision->replay_confirmed != 0u) ||
        !yyjson_mut_obj_add_str(
            document, trust_object, "basis",
            decision->checked_proof != 0u
                ? "an LRAT certificate for this exact CNF was verified by an independent checker over the original formula, and the comparison domain was shown inhabited"
                : (decision->replay_confirmed != 0u
                       ? "both functions were executed concretely on the decoded assignment and the declared relation did not hold"
                       : "nothing was checked; this outcome asserts no proof and no counterexample")) ||
        !yyjson_mut_obj_add_val(document, root, "trust", trust_object)) {
        status = QL_STATUS_OUT_OF_MEMORY;
        ql_error_set(error, status, NULL);
        goto cleanup;
    }
    if (counterexample != NULL) {
        if (!aig_sat_add_digest(document, root, "counterexample_digest",
                                &counterexample_view.digest) ||
            !yyjson_mut_obj_add_strncpy(
                document, root, "counterexample",
                (const char *)counterexample_view.data,
                counterexample_view.size)) {
            status = QL_STATUS_OUT_OF_MEMORY;
            ql_error_set(error, status, NULL);
            goto cleanup;
        }
    } else if (!aig_sat_add_digest(document, root, "counterexample_digest",
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
                                QL_AIG_SAT_OUTCOME_SCHEMA_VERSION, json,
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

static ql_status QL_CALL aig_sat_capability(
    const char *options_json, ql_proof_method_capability_v1 *capability,
    ql_error *error) {
    aig_sat_instance probe;
    ql_status status;

    memset(&probe, 0, sizeof(probe));
    probe.allocator = *ql_default_allocator();
    probe.timeout_ms = QL_AIG_SAT_DEFAULT_TIMEOUT_MS;
    status = aig_sat_parse_options(&probe, options_json, error);
    probe.allocator.deallocate(probe.allocator.user_data,
                               probe.solver_executable);
    probe.allocator.deallocate(probe.allocator.user_data,
                               probe.checker_executable);
    if (status != QL_STATUS_OK) {
        return status;
    }
    capability->family = QL_PROOF_METHOD_FAMILY_AIG_SAT;
    capability->soundness_classes = QL_PROOF_SOUNDNESS_COUNTEREXAMPLE;
    capability->result_kinds =
        QL_PROOF_RESULT_COUNTEREXAMPLE | QL_PROOF_RESULT_UNKNOWN;
    /* A proof is offered only where a certificate can actually be checked.
       Without the vendored checker this method is a refuter and says so. */
    if (ql_aig_sat_available() != 0u) {
        capability->soundness_classes |= QL_PROOF_SOUNDNESS_PROOF;
        capability->result_kinds |= QL_PROOF_RESULT_PROOF;
    }
    capability->supported_relations = QL_PROOF_RELATION_ALL;
    capability->supported_ub_policies = QL_PROOF_UB_ALL;
    /* The blaster is scalar: memory and external calls are refused by
       ql_aig_blast_smt2 rather than approximated here, so those two axes are
       not claimed at all. Claiming an axis whose only supported mode is
       "ignore" would advertise a coverage this method does not have. */
    capability->supported_observations =
        QL_OBSERVE_ALL & ~(uint64_t)(QL_OBSERVE_MEMORY |
                                     QL_OBSERVE_EXTERNAL_CALLS);
    capability->supported_memory_observations =
        QL_PROOF_MEMORY_MODE(QL_MEMORY_IGNORE);
    capability->supported_external_call_observations =
        QL_PROOF_EXTERNAL_CALL_MODE(QL_EXTERNAL_CALLS_IGNORE);
    capability->flags = QL_PROOF_CAPABILITY_PRECONDITIONS;
    ql_error_clear(error);
    return QL_STATUS_OK;
}

typedef struct aig_sat_lowered_side {
    ql_c_frontend_unit *unit;
    ql_c_lower_result *result;
    ql_ir *ir;
} aig_sat_lowered_side;

static void aig_sat_lowered_side_dispose(aig_sat_lowered_side *side) {
    ql_ir_release(side->ir);
    ql_c_lower_result_destroy(side->result);
    ql_c_frontend_unit_destroy(side->unit);
    memset(side, 0, sizeof(*side));
}

static ql_status aig_sat_lower_side(const ql_allocator *allocator,
                                    const char *source, size_t source_size,
                                    const char *name, size_t name_size,
                                    aig_sat_lowered_side *side,
                                    uint32_t *supported, ql_error *error) {
    ql_c_function_view function;
    ql_c_lower_result_view_v1 view;
    ql_status status;

    memset(side, 0, sizeof(*side));
    *supported = 0u;
    status = ql_c_frontend_analyze(allocator, source, source_size,
                                   &side->unit, error);
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
                                          side->unit, &function,
                                          &side->result, error);
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

static ql_status QL_CALL aig_sat_create(const ql_host_v1 *host,
                                        const char *options_json,
                                        void **instance, ql_error *error) {
    aig_sat_instance *created;
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
    created->timeout_ms = QL_AIG_SAT_DEFAULT_TIMEOUT_MS;
    status = aig_sat_parse_options(created, options_json, error);
    if (status != QL_STATUS_OK) {
        allocator.deallocate(allocator.user_data, created->solver_executable);
        allocator.deallocate(allocator.user_data,
                             created->checker_executable);
        allocator.deallocate(allocator.user_data, created);
        return status;
    }
    *instance = created;
    ql_error_clear(error);
    return QL_STATUS_OK;
}

static void QL_CALL aig_sat_destroy(void *instance) {
    aig_sat_instance *owned = (aig_sat_instance *)instance;
    ql_allocator allocator;

    if (owned == NULL) {
        return;
    }
    allocator = owned->allocator;
    allocator.deallocate(allocator.user_data, owned->solver_executable);
    allocator.deallocate(allocator.user_data, owned->checker_executable);
    allocator.deallocate(allocator.user_data, owned);
}

static ql_status QL_CALL aig_sat_validate(void *instance,
                                          ql_artifact *const *inputs,
                                          size_t input_count,
                                          ql_error *error) {
    ql_artifact_view view;
    ql_status status;

    (void)instance;
    if (inputs == NULL || input_count != 1u || inputs[0] == NULL) {
        ql_error_set(error, QL_STATUS_INVALID_ARGUMENT,
                     "%s consumes exactly one quodlibet.problem artifact",
                     QL_AIG_SAT_METHOD_NAME);
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
                     QL_AIG_SAT_METHOD_NAME);
        return QL_STATUS_TYPE_MISMATCH;
    }
    if (view.schema_version != QL_PROBLEM_SCHEMA_VERSION_2) {
        ql_error_set(error, QL_STATUS_TYPE_MISMATCH,
                     "%s requires problem schema v2",
                     QL_AIG_SAT_METHOD_NAME);
        return QL_STATUS_TYPE_MISMATCH;
    }
    ql_error_clear(error);
    return QL_STATUS_OK;
}

static ql_status QL_CALL aig_sat_method_run(
    void *instance, const ql_run_context_v1 *context,
    ql_artifact *const *inputs, size_t input_count, ql_artifact **output,
    ql_error *error) {
    aig_sat_instance *owned = (aig_sat_instance *)instance;
    const ql_allocator *allocator;
    ql_problem *problem = NULL;
    ql_problem_view_v2 problem_view;
    ql_product_query *product = NULL;
    ql_product_query_view_v1 query_view;
    ql_artifact *counterexample = NULL;
    aig_sat_lowered_side left;
    aig_sat_lowered_side right;
    aig_sat_decision decision;
    uint32_t left_supported = 0u;
    uint32_t right_supported = 0u;
    ql_status status;

    memset(&left, 0, sizeof(left));
    memset(&right, 0, sizeof(right));
    memset(&decision, 0, sizeof(decision));
    decision.verdict = QL_VERDICT_UNKNOWN;
    decision.evidence_class = QL_EVIDENCE_UNKNOWN;
    if (owned == NULL || context == NULL || output == NULL) {
        ql_error_set(error, QL_STATUS_INVALID_ARGUMENT,
                     "instance, run context, and output are required");
        return QL_STATUS_INVALID_ARGUMENT;
    }
    *output = NULL;
    status = aig_sat_validate(instance, inputs, input_count, error);
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
    status = aig_sat_lower_side(allocator, problem_view.left_source,
                                problem_view.left_source_size,
                                problem_view.left_function_name,
                                problem_view.left_function_name_size, &left,
                                &left_supported, error);
    if (status == QL_STATUS_OK) {
        status = aig_sat_lower_side(allocator, problem_view.right_source,
                                    problem_view.right_source_size,
                                    problem_view.right_function_name,
                                    problem_view.right_function_name_size,
                                    &right, &right_supported, error);
    }
    if (status != QL_STATUS_OK) {
        goto cleanup;
    }
    memset(&query_view, 0, sizeof(query_view));
    query_view.struct_size = sizeof(query_view);
    if (left_supported == 0u || right_supported == 0u) {
        aig_sat_set_diagnostic(
            &decision,
            "the semantic C lowering does not support one of the two functions, so no relation was encoded");
        query_view.relation = problem_view.contract.relation;
        query_view.ub_policy = problem_view.contract.ub_policy;
        query_view.covered_observations = problem_view.contract.observations;
        status = aig_sat_build_outcome(allocator, owned, &problem_view,
                                       &query_view, &decision, NULL, output,
                                       error);
        goto cleanup;
    }
    status = ql_product_query_build(allocator, problem, left.ir, right.ir,
                                    &product, error);
    if (status == QL_STATUS_TYPE_MISMATCH) {
        aig_sat_set_diagnostic(&decision, error->message);
        query_view.relation = problem_view.contract.relation;
        query_view.ub_policy = problem_view.contract.ub_policy;
        query_view.covered_observations = problem_view.contract.observations;
        status = aig_sat_build_outcome(allocator, owned, &problem_view,
                                       &query_view, &decision, NULL, output,
                                       error);
        goto cleanup;
    }
    if (status != QL_STATUS_OK) {
        goto cleanup;
    }
    status = ql_product_query_get_view(product, &query_view, error);
    if (status != QL_STATUS_OK) {
        goto cleanup;
    }
    status = aig_sat_decide(allocator, owned, context, problem, product,
                            &query_view, left.ir, right.ir, &decision,
                            &counterexample, error);
    if (status != QL_STATUS_OK) {
        goto cleanup;
    }
    status = aig_sat_build_outcome(allocator, owned, &problem_view,
                                   &query_view, &decision, counterexample,
                                   output, error);

cleanup:
    ql_artifact_release(counterexample);
    ql_product_query_destroy(product);
    aig_sat_lowered_side_dispose(&left);
    aig_sat_lowered_side_dispose(&right);
    ql_problem_release(problem);
    if (status == QL_STATUS_OK) {
        ql_error_clear(error);
    }
    return status;
}

static const ql_method_v1 aig_sat_method = {
    sizeof(ql_method_v1),
    QL_ABI_VERSION,
    QL_AIG_SAT_METHOD_NAME,
    "Bit-blasted SAT proof of the product program with an externally checked LRAT certificate",
    QL_ARTIFACT_KIND_OUTCOME,
    /* Not deterministic: a wall-clock deadline can turn the same question
       into UNKNOWN on a loaded machine. Cacheable all the same, because the
       key names the query, the tools, and the budget. */
    QL_METHOD_PROOF_PRODUCER | QL_METHOD_COUNTEREXAMPLE_PRODUCER |
        QL_METHOD_CACHEABLE,
    1u,
    1u,
    aig_sat_create,
    aig_sat_validate,
    aig_sat_method_run,
    aig_sat_destroy,
    { NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL }
};

static const ql_proof_method_v1 aig_sat_descriptor = {
    sizeof(ql_proof_method_v1),
    QL_ABI_VERSION,
    QL_PROOF_METHOD_FAMILY_AIG_SAT,
    &aig_sat_method,
    aig_sat_capability,
    { NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL }
};

const ql_method_v1 *QL_CALL ql_aig_sat_method(void) {
    return &aig_sat_method;
}

const ql_proof_method_v1 *QL_CALL ql_aig_sat_proof_method(void) {
    return &aig_sat_descriptor;
}

ql_status QL_CALL ql_aig_sat_register_method(ql_registry *registry,
                                             ql_error *error) {
    ql_status status = ql_registry_register(registry, &aig_sat_method, error);
    if (status != QL_STATUS_OK) {
        return status;
    }
    return ql_registry_register_proof_method(registry, &aig_sat_descriptor,
                                             error);
}

/* --- Outcome reading ------------------------------------------------------ */

static int aig_sat_read_digest_field(yyjson_val *object, const char *key,
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
        unsigned int byte = 0u;
        size_t nibble;
        for (nibble = 0u; nibble < 2u; ++nibble) {
            const char character = text[index * 2u + nibble];
            unsigned int value_of;
            if (character >= '0' && character <= '9') {
                value_of = (unsigned int)(character - '0');
            } else if (character >= 'a' && character <= 'f') {
                value_of = (unsigned int)(character - 'a') + 10u;
            } else {
                return 0;
            }
            byte = (byte << 4u) | value_of;
        }
        digest->bytes[index] = (uint8_t)byte;
    }
    return 1;
}

static ql_verdict aig_sat_verdict_parse(const char *text) {
    ql_verdict verdict;
    if (text == NULL) {
        return QL_VERDICT_UNKNOWN;
    }
    for (verdict = QL_VERDICT_UNKNOWN; verdict <= QL_VERDICT_BOUNDED_CLEAN;
         ++verdict) {
        if (strcmp(ql_verdict_string(verdict), text) == 0) {
            return verdict;
        }
    }
    return QL_VERDICT_UNKNOWN;
}

static ql_evidence_class aig_sat_evidence_parse(const char *text) {
    ql_evidence_class evidence;
    if (text == NULL) {
        return QL_EVIDENCE_UNKNOWN;
    }
    for (evidence = QL_EVIDENCE_PROOF; evidence <= QL_EVIDENCE_UNKNOWN;
         ++evidence) {
        if (strcmp(ql_evidence_class_string(evidence), text) == 0) {
            return evidence;
        }
    }
    return QL_EVIDENCE_UNKNOWN;
}

static ql_status aig_sat_open_outcome(const ql_artifact *artifact,
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
        view->schema_version != QL_AIG_SAT_OUTCOME_SCHEMA_VERSION) {
        ql_error_set(error, QL_STATUS_TYPE_MISMATCH,
                     "artifact is not a schema v1 quodlibet.outcome");
        return QL_STATUS_TYPE_MISMATCH;
    }
    *document = yyjson_read_opts((char *)(uintptr_t)view->data, view->size,
                                 0u, NULL, &read_error);
    if (*document == NULL) {
        ql_error_set(error, QL_STATUS_PARSE_ERROR,
                     "invalid outcome JSON at byte %zu: %s", read_error.pos,
                     read_error.msg != NULL ? read_error.msg : "parse error");
        return QL_STATUS_PARSE_ERROR;
    }
    return QL_STATUS_OK;
}

ql_status QL_CALL ql_aig_sat_outcome_read(const ql_artifact *artifact,
                                          ql_aig_sat_outcome_view_v1 *view,
                                          ql_error *error) {
    ql_artifact_view artifact_view;
    yyjson_doc *document = NULL;
    yyjson_val *root;
    yyjson_val *search_object;
    yyjson_val *trust_object;
    yyjson_val *value;
    const char *text;
    ql_status status;

    if (artifact == NULL || view == NULL ||
        view->struct_size < sizeof(*view)) {
        ql_error_set(error, QL_STATUS_INVALID_ARGUMENT,
                     "an outcome artifact and a sized view are required");
        return QL_STATUS_INVALID_ARGUMENT;
    }
    status = aig_sat_open_outcome(artifact, &artifact_view, &document, error);
    if (status != QL_STATUS_OK) {
        return status;
    }
    root = yyjson_doc_get_root(document);
    if (!yyjson_is_obj(root)) {
        yyjson_doc_free(document);
        ql_error_set(error, QL_STATUS_PARSE_ERROR,
                     "the outcome JSON is not an object");
        return QL_STATUS_PARSE_ERROR;
    }
    memset((char *)view + sizeof(view->struct_size), 0,
           sizeof(*view) - sizeof(view->struct_size));
    view->struct_size = sizeof(*view);
    view->schema_version = QL_AIG_SAT_OUTCOME_SCHEMA_VERSION;
    view->verdict =
        aig_sat_verdict_parse(yyjson_get_str(yyjson_obj_get(root, "verdict")));
    view->evidence_class = aig_sat_evidence_parse(
        yyjson_get_str(yyjson_obj_get(root, "evidence_class")));
    (void)aig_sat_read_digest_field(root, "problem_digest",
                                    &view->problem_digest);
    (void)aig_sat_read_digest_field(root, "cache_key", &view->cache_key);
    (void)aig_sat_read_digest_field(root, "counterexample_digest",
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
    search_object = yyjson_obj_get(root, "search");
    if (yyjson_is_obj(search_object)) {
        view->violation_answer = aig_sat_answer_parse(
            yyjson_get_str(yyjson_obj_get(search_object, "violation_answer")));
        view->domain_answer = aig_sat_answer_parse(
            yyjson_get_str(yyjson_obj_get(search_object, "domain_answer")));
        value = yyjson_obj_get(search_object, "cnf_variable_count");
        view->cnf_variable_count = yyjson_is_uint(value)
                                       ? yyjson_get_uint(value)
                                       : 0u;
        value = yyjson_obj_get(search_object, "cnf_clause_count");
        view->cnf_clause_count = yyjson_is_uint(value) ? yyjson_get_uint(value)
                                                       : 0u;
        (void)aig_sat_read_digest_field(search_object, "prefix_digest",
                                        &view->prefix_digest);
        (void)aig_sat_read_digest_field(search_object, "violation_digest",
                                        &view->violation_digest);
        (void)aig_sat_read_digest_field(search_object, "domain_digest",
                                        &view->domain_digest);
        (void)aig_sat_read_digest_field(search_object, "solver_binary_digest",
                                        &view->solver_binary_digest);
        (void)aig_sat_read_digest_field(search_object,
                                        "checker_binary_digest",
                                        &view->checker_binary_digest);
        (void)aig_sat_read_digest_field(search_object, "cnf_digest",
                                        &view->cnf_digest);
        (void)aig_sat_read_digest_field(search_object, "proof_digest",
                                        &view->proof_digest);
    }
    trust_object = yyjson_obj_get(root, "trust");
    if (yyjson_is_obj(trust_object)) {
        value = yyjson_obj_get(trust_object, "checked_proof");
        view->checked_proof =
            yyjson_is_bool(value) && yyjson_get_bool(value) ? 1u : 0u;
        value = yyjson_obj_get(trust_object, "replay_confirmed");
        view->replay_confirmed =
            yyjson_is_bool(value) && yyjson_get_bool(value) ? 1u : 0u;
    }
    yyjson_doc_free(document);
    ql_error_clear(error);
    return QL_STATUS_OK;
}

ql_status QL_CALL ql_aig_sat_outcome_counterexample(
    const ql_allocator *allocator, const ql_artifact *artifact,
    ql_artifact **output, ql_error *error) {
    ql_artifact_view artifact_view;
    yyjson_doc *document = NULL;
    yyjson_val *root;
    yyjson_val *value;
    ql_status status;

    if (output == NULL) {
        ql_error_set(error, QL_STATUS_INVALID_ARGUMENT,
                     "an output artifact pointer is required");
        return QL_STATUS_INVALID_ARGUMENT;
    }
    *output = NULL;
    status = aig_sat_open_outcome(artifact, &artifact_view, &document, error);
    if (status != QL_STATUS_OK) {
        return status;
    }
    root = yyjson_doc_get_root(document);
    value = yyjson_is_obj(root) ? yyjson_obj_get(root, "counterexample")
                                : NULL;
    if (!yyjson_is_str(value)) {
        yyjson_doc_free(document);
        ql_error_clear(error);
        return QL_STATUS_OK;
    }
    status = ql_artifact_create(allocator, QL_ARTIFACT_KIND_COUNTEREXAMPLE,
                                QL_AIG_SAT_OUTCOME_SCHEMA_VERSION,
                                yyjson_get_str(value), yyjson_get_len(value),
                                output, error);
    yyjson_doc_free(document);
    return status;
}
