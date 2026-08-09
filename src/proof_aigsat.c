#include "quodlibet/proof_aigsat.h"

#include <stdio.h>
#include <string.h>

#include "quodlibet/solver.h"

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
