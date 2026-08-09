#include "quodlibet/ir_interp.h"

#include "quodlibet/allocator.h"

#include <string.h>

/* Restoring division shifts the running remainder one bit past the operand
   width, so the internal representation carries one word more than the
   advertised maximum width. */
#define INTERP_WORDS 5u

typedef struct interp_bits {
    uint64_t words[INTERP_WORDS];
} interp_bits;

typedef struct interp_value {
    interp_bits bits;
    uint32_t width;
    ql_ir_type_id type;
    uint8_t defined;
    uint8_t bound;
} interp_value;

typedef struct interp_context {
    const ql_ir *ir;
    ql_ir_view_v1 view;
    ql_ir_interp_result_v1 *result;
    interp_value *values;
    uint64_t step_limit;
} interp_context;

/* ------------------------------------------------------------------ */
/* fixed-capacity bit-vector arithmetic                                */
/* ------------------------------------------------------------------ */

static void bits_zero(interp_bits *value) {
    memset(value, 0, sizeof(*value));
}

static int bits_test(const interp_bits *value, uint32_t index) {
    if (index >= INTERP_WORDS * 64u) {
        return 0;
    }
    return (value->words[index / 64u] & (UINT64_C(1) << (index % 64u))) != 0u;
}

static void bits_set(interp_bits *value, uint32_t index) {
    if (index < INTERP_WORDS * 64u) {
        value->words[index / 64u] |= UINT64_C(1) << (index % 64u);
    }
}

static void bits_mask(interp_bits *value, uint32_t width) {
    size_t index;
    for (index = 0u; index < INTERP_WORDS; ++index) {
        uint32_t low = (uint32_t)index * 64u;
        if (width <= low) {
            value->words[index] = 0u;
        } else if (width < low + 64u) {
            value->words[index] &=
                (UINT64_C(1) << (width - low)) - UINT64_C(1);
        }
    }
}

static int bits_is_zero(const interp_bits *value) {
    size_t index;
    for (index = 0u; index < INTERP_WORDS; ++index) {
        if (value->words[index] != 0u) {
            return 0;
        }
    }
    return 1;
}

static int bits_equal(const interp_bits *left, const interp_bits *right) {
    size_t index;
    for (index = 0u; index < INTERP_WORDS; ++index) {
        if (left->words[index] != right->words[index]) {
            return 0;
        }
    }
    return 1;
}

static int bits_unsigned_less(const interp_bits *left,
                              const interp_bits *right) {
    size_t index;
    for (index = INTERP_WORDS; index-- > 0u;) {
        if (left->words[index] != right->words[index]) {
            return left->words[index] < right->words[index];
        }
    }
    return 0;
}

static void bits_not(const interp_bits *value, interp_bits *out) {
    size_t index;
    for (index = 0u; index < INTERP_WORDS; ++index) {
        out->words[index] = ~value->words[index];
    }
}

static void bits_add(const interp_bits *left, const interp_bits *right,
                     interp_bits *out) {
    uint64_t carry = 0u;
    size_t index;
    for (index = 0u; index < INTERP_WORDS; ++index) {
        uint64_t sum = left->words[index] + right->words[index];
        uint64_t next = sum < left->words[index] ? 1u : 0u;
        sum += carry;
        if (sum < carry) {
            next = 1u;
        }
        out->words[index] = sum;
        carry = next;
    }
}

static void bits_sub(const interp_bits *left, const interp_bits *right,
                     interp_bits *out) {
    uint64_t borrow = 0u;
    size_t index;
    for (index = 0u; index < INTERP_WORDS; ++index) {
        uint64_t high = left->words[index];
        uint64_t low = right->words[index];
        uint64_t difference = high - low;
        uint64_t next = high < low ? 1u : 0u;
        if (difference < borrow) {
            next = 1u;
        }
        difference -= borrow;
        out->words[index] = difference;
        borrow = next;
    }
}

static void bits_negate(const interp_bits *value, uint32_t width,
                        interp_bits *out) {
    interp_bits zero;
    bits_zero(&zero);
    bits_sub(&zero, value, out);
    bits_mask(out, width);
}

static void bits_shift_left(const interp_bits *value, uint32_t amount,
                            interp_bits *out) {
    interp_bits shifted;
    size_t word_shift = amount / 64u;
    uint32_t bit_shift = amount % 64u;
    size_t index;

    bits_zero(&shifted);
    if (word_shift < INTERP_WORDS) {
        for (index = INTERP_WORDS; index-- > word_shift;) {
            size_t source = index - word_shift;
            uint64_t result = value->words[source];
            if (bit_shift != 0u) {
                result <<= bit_shift;
                if (source > 0u) {
                    result |= value->words[source - 1u] >> (64u - bit_shift);
                }
            }
            shifted.words[index] = result;
        }
    }
    *out = shifted;
}

static void bits_shift_right(const interp_bits *value, uint32_t amount,
                             interp_bits *out) {
    interp_bits shifted;
    size_t word_shift = amount / 64u;
    uint32_t bit_shift = amount % 64u;
    size_t index;

    bits_zero(&shifted);
    for (index = 0u; index + word_shift < INTERP_WORDS; ++index) {
        size_t source = index + word_shift;
        uint64_t result = value->words[source];
        if (bit_shift != 0u) {
            result >>= bit_shift;
            if (source + 1u < INTERP_WORDS) {
                result |= value->words[source + 1u] << (64u - bit_shift);
            }
        }
        shifted.words[index] = result;
    }
    *out = shifted;
}

/* Schoolbook multiplication over 32-bit limbs. Each accumulation step stays
   inside 64 bits, so no wider integer type is needed. */
static void bits_multiply(const interp_bits *left, const interp_bits *right,
                          interp_bits *out) {
    uint32_t left_limbs[INTERP_WORDS * 2u];
    uint32_t right_limbs[INTERP_WORDS * 2u];
    uint32_t product[INTERP_WORDS * 2u];
    size_t index;
    size_t inner;

    for (index = 0u; index < INTERP_WORDS; ++index) {
        left_limbs[index * 2u] = (uint32_t)(left->words[index] & 0xffffffffu);
        left_limbs[index * 2u + 1u] = (uint32_t)(left->words[index] >> 32u);
        right_limbs[index * 2u] =
            (uint32_t)(right->words[index] & 0xffffffffu);
        right_limbs[index * 2u + 1u] = (uint32_t)(right->words[index] >> 32u);
    }
    memset(product, 0, sizeof(product));
    for (index = 0u; index < INTERP_WORDS * 2u; ++index) {
        uint64_t carry = 0u;
        if (left_limbs[index] == 0u) {
            continue;
        }
        for (inner = 0u; index + inner < INTERP_WORDS * 2u; ++inner) {
            uint64_t accumulator = (uint64_t)product[index + inner] +
                                   (uint64_t)left_limbs[index] *
                                       (uint64_t)right_limbs[inner] +
                                   carry;
            product[index + inner] = (uint32_t)accumulator;
            carry = accumulator >> 32u;
        }
    }
    for (index = 0u; index < INTERP_WORDS; ++index) {
        out->words[index] = (uint64_t)product[index * 2u] |
                            ((uint64_t)product[index * 2u + 1u] << 32u);
    }
}

/* Restoring division. Every call site has already excluded a zero divisor. */
static void bits_unsigned_divide(const interp_bits *dividend,
                                 const interp_bits *divisor, uint32_t width,
                                 interp_bits *quotient,
                                 interp_bits *remainder) {
    interp_bits running_quotient;
    interp_bits running_remainder;
    uint32_t index;

    bits_zero(&running_quotient);
    bits_zero(&running_remainder);
    for (index = width; index-- > 0u;) {
        bits_shift_left(&running_remainder, 1u, &running_remainder);
        if (bits_test(dividend, index)) {
            bits_set(&running_remainder, 0u);
        }
        if (!bits_unsigned_less(&running_remainder, divisor)) {
            bits_sub(&running_remainder, divisor, &running_remainder);
            bits_set(&running_quotient, index);
        }
    }
    *quotient = running_quotient;
    *remainder = running_remainder;
}

static void bits_signed_divide(const interp_bits *dividend,
                               const interp_bits *divisor, uint32_t width,
                               interp_bits *quotient,
                               interp_bits *remainder) {
    int dividend_negative = bits_test(dividend, width - 1u);
    int divisor_negative = bits_test(divisor, width - 1u);
    interp_bits left = *dividend;
    interp_bits right = *divisor;

    if (dividend_negative) {
        bits_negate(&left, width, &left);
    }
    if (divisor_negative) {
        bits_negate(&right, width, &right);
    }
    bits_unsigned_divide(&left, &right, width, quotient, remainder);
    if (dividend_negative != divisor_negative) {
        bits_negate(quotient, width, quotient);
    }
    /* C truncates toward zero, so the remainder keeps the dividend's sign. */
    if (dividend_negative) {
        bits_negate(remainder, width, remainder);
    }
}

static int bits_exceeds_word(const interp_bits *value) {
    size_t index;
    for (index = 1u; index < INTERP_WORDS; ++index) {
        if (value->words[index] != 0u) {
            return 1;
        }
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/* byte encoding                                                       */
/* ------------------------------------------------------------------ */

static size_t interp_encoded_size(uint32_t width) {
    return (size_t)(width / 8u) + (width % 8u == 0u ? 0u : 1u);
}

static int interp_bits_from_bytes(const void *data, size_t size,
                                  uint32_t width, interp_bits *out) {
    const uint8_t *bytes = (const uint8_t *)data;
    size_t index;

    if (data == NULL || width == 0u ||
        width > QL_IR_INTERP_MAX_BIT_WIDTH ||
        size != interp_encoded_size(width)) {
        return 0;
    }
    bits_zero(out);
    for (index = 0u; index < size; ++index) {
        out->words[index / 8u] |= (uint64_t)bytes[index]
                                  << ((index % 8u) * 8u);
    }
    if (width % 8u != 0u && (bytes[size - 1u] >> (width % 8u)) != 0u) {
        return 0;
    }
    return 1;
}

static void interp_bits_to_bytes(const interp_bits *value, uint32_t width,
                                 uint8_t *bytes, size_t *size) {
    size_t encoded = interp_encoded_size(width);
    size_t index;

    for (index = 0u; index < encoded; ++index) {
        bytes[index] =
            (uint8_t)((value->words[index / 8u] >> ((index % 8u) * 8u)) &
                      0xffu);
    }
    *size = encoded;
}

/* ------------------------------------------------------------------ */
/* small accessors                                                     */
/* ------------------------------------------------------------------ */

void QL_CALL ql_ir_interp_options_init(ql_ir_interp_options_v1 *options) {
    if (options == NULL) {
        return;
    }
    memset(options, 0, sizeof(*options));
    options->struct_size = sizeof(*options);
    options->schema_version = QL_IR_INTERP_SCHEMA_VERSION;
}

void QL_CALL ql_ir_interp_input_init(ql_ir_interp_input_v1 *input) {
    if (input == NULL) {
        return;
    }
    memset(input, 0, sizeof(*input));
    input->struct_size = sizeof(*input);
    input->value = QL_IR_INVALID_VALUE_ID;
}

const char *QL_CALL ql_ir_interp_outcome_string(
    ql_ir_interp_outcome outcome) {
    switch (outcome) {
    case QL_IR_INTERP_OUTCOME_RETURN: return "return";
    case QL_IR_INTERP_OUTCOME_TRAP: return "trap";
    case QL_IR_INTERP_OUTCOME_TERMINATE: return "terminate";
    case QL_IR_INTERP_OUTCOME_UNDEFINED_BEHAVIOR: return "undefined-behavior";
    case QL_IR_INTERP_OUTCOME_DIVERGE: return "diverge";
    case QL_IR_INTERP_OUTCOME_ASSUMPTION_VIOLATED:
        return "assumption-violated";
    case QL_IR_INTERP_OUTCOME_UNSUPPORTED: return "unsupported";
    case QL_IR_INTERP_OUTCOME_STEP_LIMIT: return "step-limit";
    default: return "unknown";
    }
}

const char *QL_CALL ql_ir_interp_ub_reason_string(
    ql_ir_interp_ub_reason reason) {
    switch (reason) {
    case QL_IR_INTERP_UB_NONE: return "none";
    case QL_IR_INTERP_UB_GUARD_FAILED: return "guard-failed";
    case QL_IR_INTERP_UB_GUARD_UNDEFINED: return "guard-undefined";
    case QL_IR_INTERP_UB_GUARD_INSUFFICIENT: return "guard-insufficient";
    case QL_IR_INTERP_UB_TERMINATOR: return "terminator";
    default: return "unknown";
    }
}

/* ------------------------------------------------------------------ */
/* execution                                                           */
/* ------------------------------------------------------------------ */

static int interp_stop(interp_context *context, ql_ir_interp_outcome outcome,
                       ql_ir_interp_ub_reason reason, ql_ir_block_id block,
                       ql_ir_instruction_id instruction) {
    context->result->outcome = outcome;
    context->result->ub_reason = reason;
    context->result->block = block;
    context->result->instruction = instruction;
    return 0;
}

static int interp_execute_instruction(
    interp_context *context, const ql_ir_instruction_view_v1 *instruction,
    ql_ir_block_id block, ql_ir_instruction_id id,
    ql_ir_block_id previous_block) {
    interp_value result;
    const interp_value *left = NULL;
    const interp_value *right = NULL;
    uint32_t result_width = 0u;
    uint32_t operand_width = 0u;
    int any_undefined = 0;
    size_t index;

    memset(&result, 0, sizeof(result));
    result.defined = 1u;
    result.bound = 1u;

    for (index = 0u; index < instruction->operand_count; ++index) {
        if (context->values[instruction->operands[index]].defined == 0u) {
            any_undefined = 1;
        }
    }
    if (instruction->operand_count > 0u) {
        left = &context->values[instruction->operands[0]];
        operand_width = left->width;
    }
    if (instruction->operand_count > 1u) {
        right = &context->values[instruction->operands[1]];
    }
    if (instruction->result_count > 0u) {
        const interp_value *slot =
            &context->values[instruction->results[0]];
        result_width = slot->width;
        result.width = slot->width;
        result.type = slot->type;
    }

    switch (instruction->opcode) {
    case QL_IR_OPCODE_IDENTITY:
        result.bits = left->bits;
        result.defined = left->defined;
        break;
    case QL_IR_OPCODE_PHI: {
        size_t chosen = instruction->block_operand_count;
        const interp_value *incoming;
        for (index = 0u; index < instruction->block_operand_count; ++index) {
            if (instruction->block_operands[index] == previous_block) {
                chosen = index;
                break;
            }
        }
        if (chosen == instruction->block_operand_count) {
            return interp_stop(context, QL_IR_INTERP_OUTCOME_UNSUPPORTED,
                               QL_IR_INTERP_UB_NONE, block, id);
        }
        incoming = &context->values[instruction->operands[chosen]];
        result.bits = incoming->bits;
        result.defined = incoming->defined;
        break;
    }
    case QL_IR_OPCODE_SELECT: {
        /* Only the condition and the selected arm matter. The lowering
           evaluates both arms of `&&` and `||` eagerly and short-circuits
           definedness alone, so propagating from the arm that was not taken
           would invent undefined behaviour the C does not have. */
        const interp_value *chosen;
        if (left->defined == 0u) {
            result.defined = 0u;
            break;
        }
        chosen = bits_is_zero(&left->bits)
                     ? &context->values[instruction->operands[2]]
                     : &context->values[instruction->operands[1]];
        result.bits = chosen->bits;
        result.defined = chosen->defined;
        break;
    }
    case QL_IR_OPCODE_BOOL_NOT:
        if (any_undefined) {
            result.defined = 0u;
            break;
        }
        if (bits_is_zero(&left->bits)) {
            bits_set(&result.bits, 0u);
        }
        break;
    case QL_IR_OPCODE_BV_NOT:
        if (any_undefined) {
            result.defined = 0u;
            break;
        }
        bits_not(&left->bits, &result.bits);
        bits_mask(&result.bits, result_width);
        break;
    case QL_IR_OPCODE_BV_NEG:
        if (any_undefined) {
            result.defined = 0u;
            break;
        }
        bits_negate(&left->bits, result_width, &result.bits);
        break;
    case QL_IR_OPCODE_ADD:
        if (any_undefined) {
            result.defined = 0u;
            break;
        }
        bits_add(&left->bits, &right->bits, &result.bits);
        bits_mask(&result.bits, result_width);
        break;
    case QL_IR_OPCODE_SUB:
        if (any_undefined) {
            result.defined = 0u;
            break;
        }
        bits_sub(&left->bits, &right->bits, &result.bits);
        bits_mask(&result.bits, result_width);
        break;
    case QL_IR_OPCODE_MUL:
        if (any_undefined) {
            result.defined = 0u;
            break;
        }
        bits_multiply(&left->bits, &right->bits, &result.bits);
        bits_mask(&result.bits, result_width);
        break;
    case QL_IR_OPCODE_BV_AND:
    case QL_IR_OPCODE_BV_OR:
    case QL_IR_OPCODE_BV_XOR: {
        size_t word;
        if (any_undefined) {
            result.defined = 0u;
            break;
        }
        for (word = 0u; word < INTERP_WORDS; ++word) {
            uint64_t high = left->bits.words[word];
            uint64_t low = right->bits.words[word];
            result.bits.words[word] =
                instruction->opcode == QL_IR_OPCODE_BV_AND
                    ? (high & low)
                    : (instruction->opcode == QL_IR_OPCODE_BV_OR
                           ? (high | low)
                           : (high ^ low));
        }
        bits_mask(&result.bits, result_width);
        break;
    }
    case QL_IR_OPCODE_UDIV:
    case QL_IR_OPCODE_SDIV:
    case QL_IR_OPCODE_UREM:
    case QL_IR_OPCODE_SREM: {
        interp_bits quotient;
        interp_bits remainder;
        const int is_signed = instruction->opcode == QL_IR_OPCODE_SDIV ||
                              instruction->opcode == QL_IR_OPCODE_SREM;
        const int wants_remainder =
            instruction->opcode == QL_IR_OPCODE_UREM ||
            instruction->opcode == QL_IR_OPCODE_SREM;
        if (any_undefined || bits_is_zero(&right->bits)) {
            result.defined = 0u;
            break;
        }
        if (is_signed) {
            interp_bits minimum;
            interp_bits all_ones;
            bits_zero(&minimum);
            bits_set(&minimum, result_width - 1u);
            bits_zero(&all_ones);
            bits_not(&all_ones, &all_ones);
            bits_mask(&all_ones, result_width);
            /* The quotient of the most negative value by minus one is not
               representable, which C leaves undefined. */
            if (bits_equal(&left->bits, &minimum) &&
                bits_equal(&right->bits, &all_ones)) {
                result.defined = 0u;
                break;
            }
            bits_signed_divide(&left->bits, &right->bits, result_width,
                               &quotient, &remainder);
        } else {
            bits_unsigned_divide(&left->bits, &right->bits, result_width,
                                 &quotient, &remainder);
        }
        result.bits = wants_remainder ? remainder : quotient;
        bits_mask(&result.bits, result_width);
        break;
    }
    case QL_IR_OPCODE_SHL:
    case QL_IR_OPCODE_LSHR:
    case QL_IR_OPCODE_ASHR: {
        uint64_t amount;
        if (any_undefined || bits_exceeds_word(&right->bits)) {
            result.defined = 0u;
            break;
        }
        amount = right->bits.words[0];
        if (amount >= (uint64_t)result_width) {
            result.defined = 0u;
            break;
        }
        if (instruction->opcode == QL_IR_OPCODE_SHL) {
            bits_shift_left(&left->bits, (uint32_t)amount, &result.bits);
        } else {
            bits_shift_right(&left->bits, (uint32_t)amount, &result.bits);
            if (instruction->opcode == QL_IR_OPCODE_ASHR &&
                bits_test(&left->bits, result_width - 1u)) {
                uint32_t bit;
                for (bit = result_width - (uint32_t)amount;
                     bit < result_width; ++bit) {
                    bits_set(&result.bits, bit);
                }
            }
        }
        bits_mask(&result.bits, result_width);
        break;
    }
    case QL_IR_OPCODE_EQ:
    case QL_IR_OPCODE_NE:
        if (any_undefined) {
            result.defined = 0u;
            break;
        }
        if (bits_equal(&left->bits, &right->bits) ==
            (instruction->opcode == QL_IR_OPCODE_EQ)) {
            bits_set(&result.bits, 0u);
        }
        break;
    case QL_IR_OPCODE_ULT:
    case QL_IR_OPCODE_ULE:
        if (any_undefined) {
            result.defined = 0u;
            break;
        }
        if (bits_unsigned_less(&left->bits, &right->bits) ||
            (instruction->opcode == QL_IR_OPCODE_ULE &&
             bits_equal(&left->bits, &right->bits))) {
            bits_set(&result.bits, 0u);
        }
        break;
    case QL_IR_OPCODE_SLT:
    case QL_IR_OPCODE_SLE: {
        int left_negative;
        int right_negative;
        int less;
        if (any_undefined) {
            result.defined = 0u;
            break;
        }
        left_negative = bits_test(&left->bits, operand_width - 1u);
        right_negative = bits_test(&right->bits, operand_width - 1u);
        less = left_negative != right_negative
                   ? left_negative
                   : bits_unsigned_less(&left->bits, &right->bits);
        if (less || (instruction->opcode == QL_IR_OPCODE_SLE &&
                     bits_equal(&left->bits, &right->bits))) {
            bits_set(&result.bits, 0u);
        }
        break;
    }
    case QL_IR_OPCODE_ZEXT:
        if (any_undefined) {
            result.defined = 0u;
            break;
        }
        result.bits = left->bits;
        break;
    case QL_IR_OPCODE_SEXT:
        if (any_undefined) {
            result.defined = 0u;
            break;
        }
        result.bits = left->bits;
        if (bits_test(&left->bits, operand_width - 1u)) {
            uint32_t bit;
            for (bit = operand_width; bit < result_width; ++bit) {
                bits_set(&result.bits, bit);
            }
        }
        bits_mask(&result.bits, result_width);
        break;
    case QL_IR_OPCODE_TRUNC:
        if (any_undefined) {
            result.defined = 0u;
            break;
        }
        result.bits = left->bits;
        bits_mask(&result.bits, result_width);
        break;
    case QL_IR_OPCODE_ASSUME:
        if (left->defined == 0u || bits_is_zero(&left->bits)) {
            return interp_stop(context,
                               QL_IR_INTERP_OUTCOME_ASSUMPTION_VIOLATED,
                               QL_IR_INTERP_UB_NONE, block, id);
        }
        return 1;
    case QL_IR_OPCODE_UB_GUARD:
        if (left->defined == 0u) {
            return interp_stop(context,
                               QL_IR_INTERP_OUTCOME_UNDEFINED_BEHAVIOR,
                               QL_IR_INTERP_UB_GUARD_UNDEFINED, block, id);
        }
        if (bits_is_zero(&left->bits)) {
            return interp_stop(context,
                               QL_IR_INTERP_OUTCOME_UNDEFINED_BEHAVIOR,
                               QL_IR_INTERP_UB_GUARD_FAILED, block, id);
        }
        return 1;
    default:
        /* Memory, calls, event traces, pointers, and floating point are not
           modelled yet. Guessing a semantics here would be a wrong answer
           everywhere, so the run says it does not know. */
        return interp_stop(context, QL_IR_INTERP_OUTCOME_UNSUPPORTED,
                           QL_IR_INTERP_UB_NONE, block, id);
    }

    if (instruction->result_count == 1u) {
        interp_value *slot = &context->values[instruction->results[0]];
        result.width = slot->width;
        result.type = slot->type;
        *slot = result;
    }
    return 1;
}

static int interp_observe(interp_context *context, ql_ir_value_id value,
                          ql_ir_block_id block) {
    if (value == QL_IR_INVALID_VALUE_ID ||
        context->values[value].defined != 0u) {
        return 1;
    }
    /* Every guard on this path passed and an observation still read a value a
       partial operation left undefined, so the guard predicate was too weak.
       This is the sufficiency check the verifier cannot make structurally. */
    return interp_stop(context, QL_IR_INTERP_OUTCOME_UNDEFINED_BEHAVIOR,
                       QL_IR_INTERP_UB_GUARD_INSUFFICIENT, block,
                       QL_IR_INVALID_INSTRUCTION_ID);
}

static void interp_record_value(interp_context *context,
                                ql_ir_value_id value) {
    const interp_value *slot = &context->values[value];
    context->result->has_value = 1u;
    context->result->value_type = slot->type;
    interp_bits_to_bytes(&slot->bits, slot->width, context->result->value,
                         &context->result->value_size);
}

static ql_status interp_execute(interp_context *context, ql_error *error) {
    ql_ir_block_id current = context->view.entry_block;
    ql_ir_block_id previous = QL_IR_INVALID_BLOCK_ID;
    uint64_t steps = 0u;

    for (;;) {
        ql_ir_block_view_v1 block;
        size_t index;

        memset(&block, 0, sizeof(block));
        block.struct_size = sizeof(block);
        if (ql_ir_block_at(context->ir, current, &block, error) !=
            QL_STATUS_OK) {
            return QL_STATUS_INVALID_ARGUMENT;
        }
        for (index = 0u; index < block.instruction_count; ++index) {
            ql_ir_instruction_view_v1 instruction;
            if (++steps > context->step_limit) {
                context->result->steps = steps;
                (void)interp_stop(context, QL_IR_INTERP_OUTCOME_STEP_LIMIT,
                                  QL_IR_INTERP_UB_NONE, current,
                                  QL_IR_INVALID_INSTRUCTION_ID);
                return QL_STATUS_OK;
            }
            memset(&instruction, 0, sizeof(instruction));
            instruction.struct_size = sizeof(instruction);
            if (ql_ir_instruction_at(context->ir, block.instructions[index],
                                     &instruction, error) != QL_STATUS_OK) {
                return QL_STATUS_INVALID_ARGUMENT;
            }
            if (!interp_execute_instruction(context, &instruction, current,
                                            block.instructions[index],
                                            previous)) {
                context->result->steps = steps;
                return QL_STATUS_OK;
            }
        }
        context->result->steps = steps;
        switch (block.terminator.kind) {
        case QL_IR_TERMINATOR_RETURN:
            if (!interp_observe(context, block.terminator.return_value,
                                current) ||
                !interp_observe(context, block.terminator.memory, current) ||
                !interp_observe(context, block.terminator.event_trace,
                                current)) {
                return QL_STATUS_OK;
            }
            context->result->outcome = QL_IR_INTERP_OUTCOME_RETURN;
            context->result->block = current;
            if (block.terminator.return_value != QL_IR_INVALID_VALUE_ID) {
                interp_record_value(context, block.terminator.return_value);
            }
            return QL_STATUS_OK;
        case QL_IR_TERMINATOR_BRANCH:
            previous = current;
            current = block.terminator.target;
            break;
        case QL_IR_TERMINATOR_COND_BRANCH:
            if (!interp_observe(context, block.terminator.condition,
                                current)) {
                return QL_STATUS_OK;
            }
            previous = current;
            current =
                bits_is_zero(
                    &context->values[block.terminator.condition].bits)
                    ? block.terminator.false_target
                    : block.terminator.target;
            break;
        case QL_IR_TERMINATOR_TRAP:
            context->result->outcome = QL_IR_INTERP_OUTCOME_TRAP;
            context->result->block = current;
            return QL_STATUS_OK;
        case QL_IR_TERMINATOR_UNDEFINED_BEHAVIOR:
            (void)interp_stop(context,
                              QL_IR_INTERP_OUTCOME_UNDEFINED_BEHAVIOR,
                              QL_IR_INTERP_UB_TERMINATOR, current,
                              QL_IR_INVALID_INSTRUCTION_ID);
            return QL_STATUS_OK;
        case QL_IR_TERMINATOR_TERMINATE:
            if (!interp_observe(context, block.terminator.return_value,
                                current)) {
                return QL_STATUS_OK;
            }
            context->result->outcome = QL_IR_INTERP_OUTCOME_TERMINATE;
            context->result->block = current;
            if (block.terminator.return_value != QL_IR_INVALID_VALUE_ID) {
                interp_record_value(context, block.terminator.return_value);
            }
            return QL_STATUS_OK;
        case QL_IR_TERMINATOR_DIVERGE:
            context->result->outcome = QL_IR_INTERP_OUTCOME_DIVERGE;
            context->result->block = current;
            return QL_STATUS_OK;
        default:
            (void)interp_stop(context, QL_IR_INTERP_OUTCOME_UNSUPPORTED,
                              QL_IR_INTERP_UB_NONE, current,
                              QL_IR_INVALID_INSTRUCTION_ID);
            return QL_STATUS_OK;
        }
    }
}

static ql_status interp_prepare(interp_context *context,
                                const ql_ir_interp_input_v1 *inputs,
                                size_t input_count, int *modelled,
                                ql_error *error) {
    size_t index;

    *modelled = 1;
    for (index = 0u; index < context->view.value_count; ++index) {
        ql_ir_value_view_v1 view;
        ql_ir_type_view_v1 type;
        interp_value *value = &context->values[index];

        memset(&view, 0, sizeof(view));
        view.struct_size = sizeof(view);
        if (ql_ir_value_at(context->ir, index, &view, error) !=
            QL_STATUS_OK) {
            return QL_STATUS_INVALID_ARGUMENT;
        }
        memset(&type, 0, sizeof(type));
        type.struct_size = sizeof(type);
        if (ql_ir_type_at(context->ir, view.type, &type, error) !=
            QL_STATUS_OK) {
            return QL_STATUS_INVALID_ARGUMENT;
        }
        value->type = view.type;
        if (type.kind == QL_IR_TYPE_BOOL) {
            value->width = 1u;
        } else if (type.kind == QL_IR_TYPE_BIT_VECTOR &&
                   type.bit_width <= QL_IR_INTERP_MAX_BIT_WIDTH) {
            value->width = type.bit_width;
        } else {
            *modelled = 0;
            context->result->outcome = QL_IR_INTERP_OUTCOME_UNSUPPORTED;
            return QL_STATUS_OK;
        }
        if (view.definition_kind == QL_IR_VALUE_CONSTANT) {
            if (!interp_bits_from_bytes(view.constant_data,
                                        view.constant_size, value->width,
                                        &value->bits)) {
                ql_error_set(error, QL_STATUS_INVALID_ARGUMENT,
                             "IR constant %zu is not canonically encoded",
                             index);
                return QL_STATUS_INVALID_ARGUMENT;
            }
            value->defined = 1u;
            value->bound = 1u;
        }
    }
    for (index = 0u; index < input_count; ++index) {
        const ql_ir_interp_input_v1 *input = &inputs[index];
        ql_ir_value_view_v1 view;
        interp_value *value;

        if (input->struct_size != 0u && input->struct_size < sizeof(*input)) {
            ql_error_set(error, QL_STATUS_ABI_MISMATCH,
                         "interpreter input structure is too small");
            return QL_STATUS_ABI_MISMATCH;
        }
        if (input->value >= context->view.value_count) {
            ql_error_set(error, QL_STATUS_INVALID_ARGUMENT,
                         "interpreter input %zu names a missing value",
                         index);
            return QL_STATUS_INVALID_ARGUMENT;
        }
        memset(&view, 0, sizeof(view));
        view.struct_size = sizeof(view);
        if (ql_ir_value_at(context->ir, input->value, &view, error) !=
            QL_STATUS_OK) {
            return QL_STATUS_INVALID_ARGUMENT;
        }
        if (view.definition_kind != QL_IR_VALUE_PARAMETER) {
            ql_error_set(error, QL_STATUS_INVALID_ARGUMENT,
                         "interpreter input %zu does not name a parameter",
                         index);
            return QL_STATUS_INVALID_ARGUMENT;
        }
        value = &context->values[input->value];
        if (value->bound != 0u) {
            ql_error_set(error, QL_STATUS_ALREADY_EXISTS,
                         "interpreter input %zu binds a parameter twice",
                         index);
            return QL_STATUS_ALREADY_EXISTS;
        }
        if (!interp_bits_from_bytes(input->data, input->size, value->width,
                                    &value->bits)) {
            ql_error_set(error, QL_STATUS_INVALID_ARGUMENT,
                         "interpreter input %zu does not match its "
                         "parameter's type", index);
            return QL_STATUS_INVALID_ARGUMENT;
        }
        value->defined = 1u;
        value->bound = 1u;
    }
    for (index = 0u; index < context->view.value_count; ++index) {
        ql_ir_value_view_v1 view;
        memset(&view, 0, sizeof(view));
        view.struct_size = sizeof(view);
        if (ql_ir_value_at(context->ir, index, &view, error) !=
            QL_STATUS_OK) {
            return QL_STATUS_INVALID_ARGUMENT;
        }
        if (view.definition_kind == QL_IR_VALUE_PARAMETER &&
            context->values[index].bound == 0u) {
            ql_error_set(error, QL_STATUS_INVALID_ARGUMENT,
                         "parameter %zu has no interpreter input", index);
            return QL_STATUS_INVALID_ARGUMENT;
        }
    }
    return QL_STATUS_OK;
}

ql_status QL_CALL ql_ir_interp_run(const ql_allocator *allocator,
                                   const ql_ir *ir,
                                   const ql_ir_interp_input_v1 *inputs,
                                   size_t input_count,
                                   const ql_ir_interp_options_v1 *options,
                                   ql_ir_interp_result_v1 *result,
                                   ql_error *error) {
    const ql_allocator *selected = allocator;
    interp_context context;
    size_t slots;
    int modelled = 1;
    ql_status status;

    if (ir == NULL || result == NULL ||
        (inputs == NULL && input_count > 0u)) {
        ql_error_set(error, QL_STATUS_INVALID_ARGUMENT,
                     "IR module and interpreter result are required");
        return QL_STATUS_INVALID_ARGUMENT;
    }
    if (result->struct_size != 0u && result->struct_size < sizeof(*result)) {
        ql_error_set(error, QL_STATUS_ABI_MISMATCH,
                     "interpreter result structure is too small");
        return QL_STATUS_ABI_MISMATCH;
    }
    if (options != NULL && options->struct_size != 0u &&
        options->struct_size < sizeof(*options)) {
        ql_error_set(error, QL_STATUS_ABI_MISMATCH,
                     "interpreter option structure is too small");
        return QL_STATUS_ABI_MISMATCH;
    }
    memset(result, 0, sizeof(*result));
    result->struct_size = sizeof(*result);
    result->schema_version = QL_IR_INTERP_SCHEMA_VERSION;
    result->outcome = QL_IR_INTERP_OUTCOME_UNSUPPORTED;
    result->block = QL_IR_INVALID_BLOCK_ID;
    result->instruction = QL_IR_INVALID_INSTRUCTION_ID;
    result->value_type = QL_IR_INVALID_TYPE_ID;
    if (selected == NULL) {
        selected = ql_default_allocator();
    }
    if (!ql_allocator_is_valid(selected)) {
        ql_error_set(error, QL_STATUS_INVALID_ARGUMENT, "invalid allocator");
        return QL_STATUS_INVALID_ARGUMENT;
    }

    memset(&context, 0, sizeof(context));
    context.ir = ir;
    context.result = result;
    context.step_limit = options != NULL && options->step_limit != 0u
                             ? options->step_limit
                             : QL_IR_INTERP_DEFAULT_STEP_LIMIT;
    context.view.struct_size = sizeof(context.view);
    status = ql_ir_get_view(ir, &context.view, error);
    if (status != QL_STATUS_OK) {
        return status;
    }
    slots = context.view.value_count == 0u ? 1u : context.view.value_count;
    context.values = (interp_value *)selected->allocate(
        selected->user_data, slots * sizeof(interp_value));
    if (context.values == NULL) {
        ql_error_set(error, QL_STATUS_OUT_OF_MEMORY, NULL);
        return QL_STATUS_OUT_OF_MEMORY;
    }
    memset(context.values, 0, slots * sizeof(interp_value));

    status = interp_prepare(&context, inputs, input_count, &modelled, error);
    if (status == QL_STATUS_OK && modelled) {
        status = interp_execute(&context, error);
    }
    selected->deallocate(selected->user_data, context.values);
    if (status == QL_STATUS_OK) {
        ql_error_clear(error);
    }
    return status;
}
