#include "quodlibet/precondition.h"

#include <limits.h>
#include <stdio.h>
#include <string.h>

#include "yyjson.h"

#define QL_PRECONDITION_MAX_DEPTH 256u
#define QL_PRECONDITION_MAX_NODES 65535u
#define QL_PRECONDITION_DECIMAL_CAPACITY 1240u

typedef struct ql_precondition_node_record {
    ql_precondition_node_kind kind;
    ql_precondition_value_kind value_kind;
    uint32_t bit_width;
    uint32_t address_space;
    uint32_t *children;
    size_t child_count;
    uint32_t argument_index;
    uint32_t boolean_value;
    char *integer_value;
    size_t integer_value_size;
    uint32_t access;
    uint64_t alignment;
    uint32_t nullable;
    uint32_t alias_group;
    uint32_t truth_known;
    uint32_t truth_value;
} ql_precondition_node_record;

struct ql_precondition {
    ql_allocator allocator;
    ql_precondition_node_record *nodes;
    size_t node_count;
    size_t node_capacity;
    uint32_t root_node;
    char *canonical_bytes;
    size_t canonical_size;
    ql_digest signature_digest;
    ql_digest digest;
};

typedef struct ql_parse_context {
    ql_precondition *precondition;
    const ql_signature_view_v1 *signature;
    ql_error *error;
} ql_parse_context;

typedef struct ql_text_buffer {
    const ql_allocator *allocator;
    char *data;
    size_t size;
    size_t capacity;
} ql_text_buffer;

static const ql_allocator *select_allocator(const ql_allocator *allocator) {
    return allocator != NULL ? allocator : ql_default_allocator();
}

static void *json_allocate(void *context, size_t size) {
    ql_allocator *allocator = (ql_allocator *)context;
    return allocator->allocate(allocator->user_data, size);
}

static void *json_reallocate(void *context, void *pointer, size_t old_size,
                             size_t size) {
    ql_allocator *allocator = (ql_allocator *)context;
    (void)old_size;
    return allocator->reallocate(allocator->user_data, pointer, size);
}

static void json_deallocate(void *context, void *pointer) {
    ql_allocator *allocator = (ql_allocator *)context;
    allocator->deallocate(allocator->user_data, pointer);
}

static yyjson_alc make_json_allocator(ql_allocator *allocator) {
    yyjson_alc result;
    result.malloc = json_allocate;
    result.realloc = json_reallocate;
    result.free = json_deallocate;
    result.ctx = allocator;
    return result;
}

void QL_CALL ql_signature_argument_init(ql_signature_argument_v1 *argument) {
    if (argument == NULL) {
        return;
    }
    memset(argument, 0, sizeof(*argument));
    argument->struct_size = sizeof(*argument);
}

void QL_CALL ql_signature_view_init(ql_signature_view_v1 *signature) {
    if (signature == NULL) {
        return;
    }
    memset(signature, 0, sizeof(*signature));
    signature->struct_size = sizeof(*signature);
    signature->schema_version = QL_SIGNATURE_SCHEMA_VERSION;
    signature->pointer_width = 64u;
}

ql_status QL_CALL ql_signature_view_validate(
    const ql_signature_view_v1 *signature, ql_error *error) {
    size_t index;

    if (signature == NULL || signature->struct_size < sizeof(*signature)) {
        ql_error_set(error, QL_STATUS_INVALID_ARGUMENT,
                     "signature view v1 has an invalid size");
        return QL_STATUS_INVALID_ARGUMENT;
    }
    if (signature->schema_version != QL_SIGNATURE_SCHEMA_VERSION) {
        ql_error_set(error, QL_STATUS_SCHEMA_MISMATCH,
                     "unsupported signature schema version %u",
                     signature->schema_version);
        return QL_STATUS_SCHEMA_MISMATCH;
    }
    if (signature->pointer_width < 8u ||
        signature->pointer_width % 8u != 0u ||
        signature->pointer_width > QL_PRECONDITION_MAX_INTEGER_BITS) {
        ql_error_set(error, QL_STATUS_INVALID_ARGUMENT,
                     "signature pointer width must be a byte-aligned width between 8 and %u bits",
                     QL_PRECONDITION_MAX_INTEGER_BITS);
        return QL_STATUS_INVALID_ARGUMENT;
    }
    if (signature->argument_count > UINT32_MAX ||
        (signature->argument_count != 0u && signature->arguments == NULL)) {
        ql_error_set(error, QL_STATUS_INVALID_ARGUMENT,
                     "signature arguments are missing or exceed schema limits");
        return QL_STATUS_INVALID_ARGUMENT;
    }
    for (index = 0u; index < signature->argument_count; ++index) {
        const ql_signature_argument_v1 *argument =
            &signature->arguments[index];
        if (argument->struct_size < sizeof(*argument)) {
            ql_error_set(error, QL_STATUS_INVALID_ARGUMENT,
                         "signature argument %zu has an invalid size", index);
            return QL_STATUS_INVALID_ARGUMENT;
        }
        switch (argument->kind) {
        case QL_SIGNATURE_ARGUMENT_BOOL:
            if (argument->bit_width != 1u || argument->address_space != 0u) {
                ql_error_set(error, QL_STATUS_INVALID_ARGUMENT,
                             "boolean argument %zu must have width 1 and address space 0",
                             index);
                return QL_STATUS_INVALID_ARGUMENT;
            }
            break;
        case QL_SIGNATURE_ARGUMENT_SIGNED_INTEGER:
        case QL_SIGNATURE_ARGUMENT_UNSIGNED_INTEGER:
            if (argument->bit_width == 0u ||
                argument->bit_width > QL_PRECONDITION_MAX_INTEGER_BITS ||
                argument->address_space != 0u) {
                ql_error_set(error, QL_STATUS_INVALID_ARGUMENT,
                             "integer argument %zu has an invalid width or address space",
                             index);
                return QL_STATUS_INVALID_ARGUMENT;
            }
            break;
        case QL_SIGNATURE_ARGUMENT_POINTER:
            if (argument->bit_width != signature->pointer_width) {
                ql_error_set(error, QL_STATUS_INVALID_ARGUMENT,
                             "pointer argument %zu width does not match the signature pointer width",
                             index);
                return QL_STATUS_INVALID_ARGUMENT;
            }
            break;
        default:
            ql_error_set(error, QL_STATUS_INVALID_ARGUMENT,
                         "signature argument %zu has an invalid type", index);
            return QL_STATUS_INVALID_ARGUMENT;
        }
    }
    ql_error_clear(error);
    return QL_STATUS_OK;
}

static ql_precondition_value_kind argument_value_kind(
    ql_signature_argument_kind kind) {
    switch (kind) {
    case QL_SIGNATURE_ARGUMENT_BOOL: return QL_PRECONDITION_VALUE_BOOL;
    case QL_SIGNATURE_ARGUMENT_SIGNED_INTEGER:
        return QL_PRECONDITION_VALUE_SIGNED_INTEGER;
    case QL_SIGNATURE_ARGUMENT_UNSIGNED_INTEGER:
        return QL_PRECONDITION_VALUE_UNSIGNED_INTEGER;
    case QL_SIGNATURE_ARGUMENT_POINTER:
        return QL_PRECONDITION_VALUE_POINTER;
    default: return QL_PRECONDITION_VALUE_INVALID;
    }
}

static int checked_multiply_size(size_t left, size_t right, size_t *result) {
    if (left != 0u && right > SIZE_MAX / left) {
        return 0;
    }
    *result = left * right;
    return 1;
}

static ql_status grow_nodes(ql_precondition *precondition, size_t required,
                            ql_error *error) {
    size_t capacity;
    size_t bytes;
    void *resized;

    if (required <= precondition->node_capacity) {
        return QL_STATUS_OK;
    }
    capacity = precondition->node_capacity != 0u
                   ? precondition->node_capacity
                   : 32u;
    while (capacity < required) {
        if (capacity > QL_PRECONDITION_MAX_NODES / 2u) {
            capacity = QL_PRECONDITION_MAX_NODES;
            break;
        }
        capacity *= 2u;
    }
    if (capacity < required ||
        !checked_multiply_size(capacity, sizeof(*precondition->nodes),
                               &bytes)) {
        ql_error_set(error, QL_STATUS_OUT_OF_MEMORY,
                     "precondition AST exceeds schema limits");
        return QL_STATUS_OUT_OF_MEMORY;
    }
    resized = precondition->allocator.reallocate(
        precondition->allocator.user_data, precondition->nodes, bytes);
    if (resized == NULL) {
        ql_error_set(error, QL_STATUS_OUT_OF_MEMORY, NULL);
        return QL_STATUS_OUT_OF_MEMORY;
    }
    precondition->nodes = (ql_precondition_node_record *)resized;
    precondition->node_capacity = capacity;
    return QL_STATUS_OK;
}

static ql_status add_node(ql_parse_context *context,
                          const ql_precondition_node_record *node,
                          uint32_t *index) {
    ql_status status;

    if (context->precondition->node_count >= QL_PRECONDITION_MAX_NODES) {
        ql_error_set(context->error, QL_STATUS_PARSE_ERROR,
                     "precondition has more than %u AST nodes",
                     QL_PRECONDITION_MAX_NODES);
        return QL_STATUS_PARSE_ERROR;
    }
    status = grow_nodes(context->precondition,
                        context->precondition->node_count + 1u,
                        context->error);
    if (status != QL_STATUS_OK) {
        return status;
    }
    *index = (uint32_t)context->precondition->node_count;
    context->precondition->nodes[context->precondition->node_count] = *node;
    ++context->precondition->node_count;
    return QL_STATUS_OK;
}

static ql_status copy_children(ql_parse_context *context,
                               const uint32_t *children, size_t child_count,
                               uint32_t **copy) {
    size_t bytes;

    *copy = NULL;
    if (child_count == 0u) {
        return QL_STATUS_OK;
    }
    if (!checked_multiply_size(child_count, sizeof(**copy), &bytes)) {
        ql_error_set(context->error, QL_STATUS_OUT_OF_MEMORY, NULL);
        return QL_STATUS_OUT_OF_MEMORY;
    }
    *copy = context->precondition->allocator.allocate(
        context->precondition->allocator.user_data, bytes);
    if (*copy == NULL) {
        ql_error_set(context->error, QL_STATUS_OUT_OF_MEMORY, NULL);
        return QL_STATUS_OUT_OF_MEMORY;
    }
    memcpy(*copy, children, bytes);
    return QL_STATUS_OK;
}

static int json_string_equals(yyjson_val *value, const char *expected) {
    size_t expected_size = strlen(expected);
    return yyjson_is_str(value) && yyjson_get_len(value) == expected_size &&
           memcmp(yyjson_get_str(value), expected, expected_size) == 0;
}

static ql_status require_object_fields(yyjson_val *object,
                                       const char *context_name,
                                       const char *const *names,
                                       size_t name_count, ql_error *error) {
    uint64_t seen = 0u;
    uint64_t expected;
    size_t object_index;
    size_t object_maximum;
    yyjson_val *key;
    yyjson_val *value;

    if (!yyjson_is_obj(object) || name_count == 0u || name_count > 63u) {
        ql_error_set(error, QL_STATUS_SCHEMA_MISMATCH,
                     "%s must be an object", context_name);
        return QL_STATUS_SCHEMA_MISMATCH;
    }
    yyjson_obj_foreach(object, object_index, object_maximum, key, value) {
        const char *key_text = yyjson_get_str(key);
        size_t key_size = yyjson_get_len(key);
        size_t name_index;
        uint64_t bit = 0u;
        (void)value;

        for (name_index = 0u; name_index < name_count; ++name_index) {
            size_t known_size = strlen(names[name_index]);
            if (key_size == known_size &&
                memcmp(key_text, names[name_index], known_size) == 0) {
                bit = UINT64_C(1) << name_index;
                break;
            }
        }
        if (bit == 0u) {
            ql_error_set(error, QL_STATUS_SCHEMA_MISMATCH,
                         "%s has an unknown field", context_name);
            return QL_STATUS_SCHEMA_MISMATCH;
        }
        if ((seen & bit) != 0u) {
            ql_error_set(error, QL_STATUS_SCHEMA_MISMATCH,
                         "%s has a duplicate field", context_name);
            return QL_STATUS_SCHEMA_MISMATCH;
        }
        seen |= bit;
    }
    expected = (UINT64_C(1) << name_count) - 1u;
    if (seen != expected) {
        ql_error_set(error, QL_STATUS_SCHEMA_MISMATCH,
                     "%s is missing a required field", context_name);
        return QL_STATUS_SCHEMA_MISMATCH;
    }
    return QL_STATUS_OK;
}

static int get_uint32_value(yyjson_val *value, uint32_t *result) {
    uint64_t number;
    if (!yyjson_is_uint(value)) {
        return 0;
    }
    number = yyjson_get_uint(value);
    if (number > UINT32_MAX) {
        return 0;
    }
    *result = (uint32_t)number;
    return 1;
}

static int is_power_of_two(uint64_t value) {
    return value != 0u && (value & (value - 1u)) == 0u;
}

static int alignment_fits_width(uint64_t alignment, uint32_t width) {
    if (width >= 64u) {
        return 1;
    }
    return alignment < (UINT64_C(1) << width);
}

static void decimal_power_limit(uint32_t exponent, int subtract_one,
                                char output[QL_PRECONDITION_DECIMAL_CAPACITY],
                                size_t *output_size) {
    uint8_t digits[QL_PRECONDITION_DECIMAL_CAPACITY];
    size_t digit_count = 1u;
    uint32_t step;
    size_t index;

    memset(digits, 0, sizeof(digits));
    digits[0] = 1u;
    for (step = 0u; step < exponent; ++step) {
        uint32_t carry = 0u;
        for (index = 0u; index < digit_count; ++index) {
            uint32_t value = (uint32_t)digits[index] * 2u + carry;
            digits[index] = (uint8_t)(value % 10u);
            carry = value / 10u;
        }
        if (carry != 0u) {
            digits[digit_count++] = (uint8_t)carry;
        }
    }
    if (subtract_one != 0) {
        index = 0u;
        while (digits[index] == 0u) {
            digits[index] = 9u;
            ++index;
        }
        --digits[index];
        while (digit_count > 1u && digits[digit_count - 1u] == 0u) {
            --digit_count;
        }
    }
    for (index = 0u; index < digit_count; ++index) {
        output[index] = (char)('0' + digits[digit_count - index - 1u]);
    }
    output[digit_count] = '\0';
    *output_size = digit_count;
}

static int decimal_magnitude_fits(const char *digits, size_t digit_count,
                                  uint32_t width, int is_signed,
                                  int is_negative) {
    char limit[QL_PRECONDITION_DECIMAL_CAPACITY];
    size_t limit_size;
    uint32_t exponent;
    int subtract_one;

    if (is_signed != 0) {
        exponent = width - 1u;
        subtract_one = is_negative == 0;
    } else {
        exponent = width;
        subtract_one = 1;
    }
    decimal_power_limit(exponent, subtract_one, limit, &limit_size);
    if (digit_count != limit_size) {
        return digit_count < limit_size;
    }
    return memcmp(digits, limit, digit_count) <= 0;
}

static ql_status normalize_integer(ql_parse_context *context,
                                   const char *input, size_t input_size,
                                   uint32_t width, int is_signed,
                                   char **output, size_t *output_size) {
    size_t position = 0u;
    size_t first_digit;
    size_t digit_count;
    int negative = 0;
    size_t result_size;
    char *result;
    size_t index;

    *output = NULL;
    *output_size = 0u;
    if (input == NULL || input_size == 0u ||
        memchr(input, '\0', input_size) != NULL) {
        ql_error_set(context->error, QL_STATUS_TYPE_MISMATCH,
                     "integer constant must be a nonempty decimal string");
        return QL_STATUS_TYPE_MISMATCH;
    }
    if (input[position] == '-' || input[position] == '+') {
        negative = input[position] == '-';
        ++position;
    }
    if (position == input_size) {
        ql_error_set(context->error, QL_STATUS_TYPE_MISMATCH,
                     "integer constant has no digits");
        return QL_STATUS_TYPE_MISMATCH;
    }
    for (index = position; index < input_size; ++index) {
        if (input[index] < '0' || input[index] > '9') {
            ql_error_set(context->error, QL_STATUS_TYPE_MISMATCH,
                         "integer constant must use base-10 digits");
            return QL_STATUS_TYPE_MISMATCH;
        }
    }
    while (position + 1u < input_size && input[position] == '0') {
        ++position;
    }
    if (input[position] == '0' && position + 1u == input_size) {
        negative = 0;
    }
    if (negative != 0 && is_signed == 0) {
        ql_error_set(context->error, QL_STATUS_TYPE_MISMATCH,
                     "unsigned integer constant cannot be negative");
        return QL_STATUS_TYPE_MISMATCH;
    }
    first_digit = position;
    digit_count = input_size - first_digit;
    if (!decimal_magnitude_fits(input + first_digit, digit_count, width,
                                is_signed, negative)) {
        ql_error_set(context->error, QL_STATUS_TYPE_MISMATCH,
                     "integer constant does not fit its declared width");
        return QL_STATUS_TYPE_MISMATCH;
    }
    result_size = digit_count + (negative != 0 ? 1u : 0u);
    if (result_size == SIZE_MAX) {
        ql_error_set(context->error, QL_STATUS_OUT_OF_MEMORY, NULL);
        return QL_STATUS_OUT_OF_MEMORY;
    }
    result = context->precondition->allocator.allocate(
        context->precondition->allocator.user_data, result_size + 1u);
    if (result == NULL) {
        ql_error_set(context->error, QL_STATUS_OUT_OF_MEMORY, NULL);
        return QL_STATUS_OUT_OF_MEMORY;
    }
    position = 0u;
    if (negative != 0) {
        result[position++] = '-';
    }
    memcpy(result + position, input + first_digit, digit_count);
    result[result_size] = '\0';
    *output = result;
    *output_size = result_size;
    return QL_STATUS_OK;
}

static ql_status parse_expression(ql_parse_context *context,
                                  yyjson_val *value, uint32_t depth,
                                  uint32_t *result);

static int nodes_equal(const ql_precondition *precondition, uint32_t left,
                       uint32_t right, uint32_t depth) {
    const ql_precondition_node_record *left_node;
    const ql_precondition_node_record *right_node;
    size_t index;

    if (left == right) {
        return 1;
    }
    if (depth > QL_PRECONDITION_MAX_DEPTH ||
        left >= precondition->node_count || right >= precondition->node_count) {
        return 0;
    }
    left_node = &precondition->nodes[left];
    right_node = &precondition->nodes[right];
    if (left_node->kind != right_node->kind ||
        left_node->value_kind != right_node->value_kind ||
        left_node->bit_width != right_node->bit_width ||
        left_node->address_space != right_node->address_space ||
        left_node->child_count != right_node->child_count ||
        left_node->argument_index != right_node->argument_index ||
        left_node->boolean_value != right_node->boolean_value ||
        left_node->integer_value_size != right_node->integer_value_size ||
        left_node->access != right_node->access ||
        left_node->alignment != right_node->alignment ||
        left_node->nullable != right_node->nullable ||
        left_node->alias_group != right_node->alias_group) {
        return 0;
    }
    if (left_node->integer_value_size != 0u) {
        if (left_node->integer_value == NULL ||
            right_node->integer_value == NULL ||
            memcmp(left_node->integer_value, right_node->integer_value,
                   left_node->integer_value_size) != 0) {
            return 0;
        }
    }
    for (index = 0u; index < left_node->child_count; ++index) {
        if (!nodes_equal(precondition, left_node->children[index],
                         right_node->children[index], depth + 1u)) {
            return 0;
        }
    }
    return 1;
}

static int integer_text_compare(const char *left, size_t left_size,
                                const char *right, size_t right_size) {
    int left_negative = left_size != 0u && left[0] == '-';
    int right_negative = right_size != 0u && right[0] == '-';
    const char *left_digits = left + (left_negative != 0 ? 1u : 0u);
    const char *right_digits = right + (right_negative != 0 ? 1u : 0u);
    size_t left_digits_size = left_size - (left_negative != 0 ? 1u : 0u);
    size_t right_digits_size =
        right_size - (right_negative != 0 ? 1u : 0u);
    int magnitude;

    if (left_negative != right_negative) {
        return left_negative != 0 ? -1 : 1;
    }
    if (left_digits_size != right_digits_size) {
        magnitude = left_digits_size < right_digits_size ? -1 : 1;
    } else {
        magnitude = memcmp(left_digits, right_digits, left_digits_size);
        magnitude = magnitude < 0 ? -1 : (magnitude > 0 ? 1 : 0);
    }
    return left_negative != 0 ? -magnitude : magnitude;
}

static void set_binary_truth(ql_precondition *precondition,
                             ql_precondition_node_record *node) {
    const ql_precondition_node_record *left =
        &precondition->nodes[node->children[0]];
    const ql_precondition_node_record *right =
        &precondition->nodes[node->children[1]];
    int comparison = 0;
    int comparison_known = 0;

    if (nodes_equal(precondition, node->children[0], node->children[1], 0u)) {
        comparison_known = 1;
        comparison = 0;
    } else if (left->kind == QL_PRECONDITION_NODE_INTEGER &&
               right->kind == QL_PRECONDITION_NODE_INTEGER) {
        comparison_known = 1;
        comparison = integer_text_compare(
            left->integer_value, left->integer_value_size,
            right->integer_value, right->integer_value_size);
    } else if (left->kind == QL_PRECONDITION_NODE_BOOL &&
               right->kind == QL_PRECONDITION_NODE_BOOL) {
        comparison_known = 1;
        comparison = left->boolean_value < right->boolean_value
                         ? -1
                         : (left->boolean_value > right->boolean_value ? 1
                                                                      : 0);
    }
    if (comparison_known == 0) {
        return;
    }
    node->truth_known = 1u;
    switch (node->kind) {
    case QL_PRECONDITION_NODE_EQUAL:
        node->truth_value = comparison == 0;
        break;
    case QL_PRECONDITION_NODE_NOT_EQUAL:
        node->truth_value = comparison != 0;
        break;
    case QL_PRECONDITION_NODE_SIGNED_LESS:
    case QL_PRECONDITION_NODE_UNSIGNED_LESS:
        node->truth_value = comparison < 0;
        break;
    case QL_PRECONDITION_NODE_SIGNED_LESS_EQUAL:
    case QL_PRECONDITION_NODE_UNSIGNED_LESS_EQUAL:
        node->truth_value = comparison <= 0;
        break;
    case QL_PRECONDITION_NODE_SIGNED_GREATER:
    case QL_PRECONDITION_NODE_UNSIGNED_GREATER:
        node->truth_value = comparison > 0;
        break;
    case QL_PRECONDITION_NODE_SIGNED_GREATER_EQUAL:
    case QL_PRECONDITION_NODE_UNSIGNED_GREATER_EQUAL:
        node->truth_value = comparison >= 0;
        break;
    default:
        node->truth_known = 0u;
        node->truth_value = 0u;
        break;
    }
}

static int comparison_kinds_are_negations(ql_precondition_node_kind left,
                                          ql_precondition_node_kind right) {
    return (left == QL_PRECONDITION_NODE_EQUAL &&
            right == QL_PRECONDITION_NODE_NOT_EQUAL) ||
           (left == QL_PRECONDITION_NODE_NOT_EQUAL &&
            right == QL_PRECONDITION_NODE_EQUAL) ||
           (left == QL_PRECONDITION_NODE_SIGNED_LESS &&
            right == QL_PRECONDITION_NODE_SIGNED_GREATER_EQUAL) ||
           (left == QL_PRECONDITION_NODE_SIGNED_GREATER_EQUAL &&
            right == QL_PRECONDITION_NODE_SIGNED_LESS) ||
           (left == QL_PRECONDITION_NODE_SIGNED_LESS_EQUAL &&
            right == QL_PRECONDITION_NODE_SIGNED_GREATER) ||
           (left == QL_PRECONDITION_NODE_SIGNED_GREATER &&
            right == QL_PRECONDITION_NODE_SIGNED_LESS_EQUAL) ||
           (left == QL_PRECONDITION_NODE_UNSIGNED_LESS &&
            right == QL_PRECONDITION_NODE_UNSIGNED_GREATER_EQUAL) ||
           (left == QL_PRECONDITION_NODE_UNSIGNED_GREATER_EQUAL &&
            right == QL_PRECONDITION_NODE_UNSIGNED_LESS) ||
           (left == QL_PRECONDITION_NODE_UNSIGNED_LESS_EQUAL &&
            right == QL_PRECONDITION_NODE_UNSIGNED_GREATER) ||
           (left == QL_PRECONDITION_NODE_UNSIGNED_GREATER &&
            right == QL_PRECONDITION_NODE_UNSIGNED_LESS_EQUAL);
}

static int nodes_are_negations(const ql_precondition *precondition,
                               uint32_t left_index, uint32_t right_index) {
    const ql_precondition_node_record *left =
        &precondition->nodes[left_index];
    const ql_precondition_node_record *right =
        &precondition->nodes[right_index];

    if (left->kind == QL_PRECONDITION_NODE_NOT &&
        nodes_equal(precondition, left->children[0], right_index, 0u)) {
        return 1;
    }
    if (right->kind == QL_PRECONDITION_NODE_NOT &&
        nodes_equal(precondition, right->children[0], left_index, 0u)) {
        return 1;
    }
    if (!comparison_kinds_are_negations(left->kind, right->kind) ||
        left->child_count != 2u || right->child_count != 2u) {
        return 0;
    }
    return nodes_equal(precondition, left->children[0], right->children[0],
                       0u) &&
           nodes_equal(precondition, left->children[1], right->children[1],
                       0u);
}

static int valid_ranges_have_empty_alias_intersection(
    const ql_precondition *precondition, uint32_t left_index,
    uint32_t right_index) {
    const ql_precondition_node_record *left =
        &precondition->nodes[left_index];
    const ql_precondition_node_record *right =
        &precondition->nodes[right_index];

    if (left->kind != QL_PRECONDITION_NODE_VALID_RANGE ||
        right->kind != QL_PRECONDITION_NODE_VALID_RANGE ||
        left->alias_group == 0u || right->alias_group == 0u ||
        left->alias_group == right->alias_group) {
        return 0;
    }
    return nodes_equal(precondition, left->children[0], right->children[0],
                       0u);
}

static void set_nary_truth(ql_precondition *precondition,
                           ql_precondition_node_record *node) {
    size_t index;
    int all_known = 1;

    if (node->kind == QL_PRECONDITION_NODE_AND) {
        for (index = 0u; index < node->child_count; ++index) {
            const ql_precondition_node_record *child =
                &precondition->nodes[node->children[index]];
            size_t other;
            if (child->truth_known != 0u && child->truth_value == 0u) {
                node->truth_known = 1u;
                node->truth_value = 0u;
                return;
            }
            if (child->truth_known == 0u) {
                all_known = 0;
            }
            for (other = index + 1u; other < node->child_count; ++other) {
                if (nodes_are_negations(precondition, node->children[index],
                                        node->children[other]) ||
                    valid_ranges_have_empty_alias_intersection(
                        precondition, node->children[index],
                        node->children[other])) {
                    node->truth_known = 1u;
                    node->truth_value = 0u;
                    return;
                }
            }
        }
        if (all_known != 0) {
            node->truth_known = 1u;
            node->truth_value = 1u;
        }
        return;
    }
    for (index = 0u; index < node->child_count; ++index) {
        const ql_precondition_node_record *child =
            &precondition->nodes[node->children[index]];
        if (child->truth_known != 0u && child->truth_value != 0u) {
            node->truth_known = 1u;
            node->truth_value = 1u;
            return;
        }
        if (child->truth_known == 0u) {
            all_known = 0;
        }
    }
    if (all_known != 0) {
        node->truth_known = 1u;
        node->truth_value = 0u;
    }
}

static ql_status parse_boolean_literal(ql_parse_context *context,
                                       yyjson_val *value, uint32_t *result) {
    ql_precondition_node_record node;
    memset(&node, 0, sizeof(node));
    node.kind = QL_PRECONDITION_NODE_BOOL;
    node.value_kind = QL_PRECONDITION_VALUE_BOOL;
    node.bit_width = 1u;
    node.boolean_value = yyjson_get_bool(value) ? 1u : 0u;
    node.truth_known = 1u;
    node.truth_value = node.boolean_value;
    return add_node(context, &node, result);
}

static ql_status parse_argument(ql_parse_context *context, yyjson_val *value,
                                uint32_t *result) {
    static const char *const fields[] = {"op", "index"};
    ql_precondition_node_record node;
    yyjson_val *index_value;
    uint32_t argument_index;
    const ql_signature_argument_v1 *argument;
    ql_status status = require_object_fields(
        value, "argument expression", fields,
        sizeof(fields) / sizeof(fields[0]), context->error);
    if (status != QL_STATUS_OK) {
        return status;
    }
    index_value = yyjson_obj_get(value, "index");
    if (!get_uint32_value(index_value, &argument_index) ||
        (size_t)argument_index >= context->signature->argument_count) {
        ql_error_set(context->error, QL_STATUS_TYPE_MISMATCH,
                     "argument index is outside the function signature");
        return QL_STATUS_TYPE_MISMATCH;
    }
    argument = &context->signature->arguments[argument_index];
    memset(&node, 0, sizeof(node));
    node.kind = QL_PRECONDITION_NODE_ARGUMENT;
    node.value_kind = argument_value_kind(argument->kind);
    node.bit_width = argument->bit_width;
    node.address_space = argument->address_space;
    node.argument_index = argument_index;
    return add_node(context, &node, result);
}

static ql_status parse_integer(ql_parse_context *context, yyjson_val *value,
                               uint32_t *result) {
    static const char *const fields[] = {"op", "signed", "width", "value"};
    ql_precondition_node_record node;
    yyjson_val *signed_value;
    yyjson_val *width_value;
    yyjson_val *text_value;
    uint32_t width;
    int is_signed;
    ql_status status = require_object_fields(
        value, "integer expression", fields,
        sizeof(fields) / sizeof(fields[0]), context->error);
    if (status != QL_STATUS_OK) {
        return status;
    }
    signed_value = yyjson_obj_get(value, "signed");
    width_value = yyjson_obj_get(value, "width");
    text_value = yyjson_obj_get(value, "value");
    if (!yyjson_is_bool(signed_value) ||
        !get_uint32_value(width_value, &width) || width == 0u ||
        width > QL_PRECONDITION_MAX_INTEGER_BITS ||
        !yyjson_is_str(text_value)) {
        ql_error_set(context->error, QL_STATUS_TYPE_MISMATCH,
                     "integer expression has an invalid signedness, width, or value");
        return QL_STATUS_TYPE_MISMATCH;
    }
    is_signed = yyjson_get_bool(signed_value) ? 1 : 0;
    memset(&node, 0, sizeof(node));
    node.kind = QL_PRECONDITION_NODE_INTEGER;
    node.value_kind = is_signed != 0
                          ? QL_PRECONDITION_VALUE_SIGNED_INTEGER
                          : QL_PRECONDITION_VALUE_UNSIGNED_INTEGER;
    node.bit_width = width;
    status = normalize_integer(context, yyjson_get_str(text_value),
                               yyjson_get_len(text_value), width, is_signed,
                               &node.integer_value,
                               &node.integer_value_size);
    if (status != QL_STATUS_OK) {
        return status;
    }
    status = add_node(context, &node, result);
    if (status != QL_STATUS_OK) {
        context->precondition->allocator.deallocate(
            context->precondition->allocator.user_data, node.integer_value);
    }
    return status;
}

static ql_status parse_not(ql_parse_context *context, yyjson_val *value,
                           uint32_t depth, uint32_t *result) {
    static const char *const fields[] = {"op", "value"};
    ql_precondition_node_record node;
    uint32_t child;
    ql_status status = require_object_fields(
        value, "not expression", fields, sizeof(fields) / sizeof(fields[0]),
        context->error);
    if (status != QL_STATUS_OK) {
        return status;
    }
    status = parse_expression(context, yyjson_obj_get(value, "value"),
                              depth + 1u, &child);
    if (status != QL_STATUS_OK) {
        return status;
    }
    if (context->precondition->nodes[child].value_kind !=
        QL_PRECONDITION_VALUE_BOOL) {
        ql_error_set(context->error, QL_STATUS_TYPE_MISMATCH,
                     "not operand must be boolean");
        return QL_STATUS_TYPE_MISMATCH;
    }
    memset(&node, 0, sizeof(node));
    node.kind = QL_PRECONDITION_NODE_NOT;
    node.value_kind = QL_PRECONDITION_VALUE_BOOL;
    node.bit_width = 1u;
    node.child_count = 1u;
    status = copy_children(context, &child, 1u, &node.children);
    if (status != QL_STATUS_OK) {
        return status;
    }
    if (context->precondition->nodes[child].truth_known != 0u) {
        node.truth_known = 1u;
        node.truth_value =
            context->precondition->nodes[child].truth_value == 0u;
    }
    status = add_node(context, &node, result);
    if (status != QL_STATUS_OK) {
        context->precondition->allocator.deallocate(
            context->precondition->allocator.user_data, node.children);
    }
    return status;
}

static ql_status parse_nary_boolean(ql_parse_context *context,
                                    yyjson_val *value, uint32_t depth,
                                    ql_precondition_node_kind kind,
                                    uint32_t *result) {
    static const char *const fields[] = {"op", "args"};
    ql_precondition_node_record node;
    yyjson_val *arguments;
    yyjson_val *argument;
    size_t index;
    size_t maximum;
    size_t argument_count;
    size_t bytes;
    uint32_t *children;
    ql_status status = require_object_fields(
        value, kind == QL_PRECONDITION_NODE_AND ? "and expression"
                                                : "or expression",
        fields, sizeof(fields) / sizeof(fields[0]), context->error);
    if (status != QL_STATUS_OK) {
        return status;
    }
    arguments = yyjson_obj_get(value, "args");
    if (!yyjson_is_arr(arguments) || yyjson_arr_size(arguments) < 2u ||
        yyjson_arr_size(arguments) > QL_PRECONDITION_MAX_NODES) {
        ql_error_set(context->error, QL_STATUS_SCHEMA_MISMATCH,
                     "and/or args must contain at least two expressions");
        return QL_STATUS_SCHEMA_MISMATCH;
    }
    argument_count = yyjson_arr_size(arguments);
    if (!checked_multiply_size(argument_count, sizeof(*children), &bytes)) {
        ql_error_set(context->error, QL_STATUS_OUT_OF_MEMORY, NULL);
        return QL_STATUS_OUT_OF_MEMORY;
    }
    children = context->precondition->allocator.allocate(
        context->precondition->allocator.user_data, bytes);
    if (children == NULL) {
        ql_error_set(context->error, QL_STATUS_OUT_OF_MEMORY, NULL);
        return QL_STATUS_OUT_OF_MEMORY;
    }
    yyjson_arr_foreach(arguments, index, maximum, argument) {
        status = parse_expression(context, argument, depth + 1u,
                                  &children[index]);
        if (status != QL_STATUS_OK) {
            context->precondition->allocator.deallocate(
                context->precondition->allocator.user_data, children);
            return status;
        }
        if (context->precondition->nodes[children[index]].value_kind !=
            QL_PRECONDITION_VALUE_BOOL) {
            context->precondition->allocator.deallocate(
                context->precondition->allocator.user_data, children);
            ql_error_set(context->error, QL_STATUS_TYPE_MISMATCH,
                         "and/or operands must be boolean");
            return QL_STATUS_TYPE_MISMATCH;
        }
    }
    memset(&node, 0, sizeof(node));
    node.kind = kind;
    node.value_kind = QL_PRECONDITION_VALUE_BOOL;
    node.bit_width = 1u;
    node.children = children;
    node.child_count = argument_count;
    set_nary_truth(context->precondition, &node);
    status = add_node(context, &node, result);
    if (status != QL_STATUS_OK) {
        context->precondition->allocator.deallocate(
            context->precondition->allocator.user_data, children);
    }
    return status;
}

static int value_types_equal(const ql_precondition_node_record *left,
                             const ql_precondition_node_record *right) {
    return left->value_kind == right->value_kind &&
           left->bit_width == right->bit_width &&
           left->address_space == right->address_space;
}

static int kind_is_signed_comparison(ql_precondition_node_kind kind) {
    return kind >= QL_PRECONDITION_NODE_SIGNED_LESS &&
           kind <= QL_PRECONDITION_NODE_SIGNED_GREATER_EQUAL;
}

static int kind_is_unsigned_comparison(ql_precondition_node_kind kind) {
    return kind >= QL_PRECONDITION_NODE_UNSIGNED_LESS &&
           kind <= QL_PRECONDITION_NODE_UNSIGNED_GREATER_EQUAL;
}

static int kind_is_signed_arithmetic(ql_precondition_node_kind kind) {
    return kind >= QL_PRECONDITION_NODE_SIGNED_ADD &&
           kind <= QL_PRECONDITION_NODE_SIGNED_MULTIPLY;
}

static int kind_is_unsigned_arithmetic(ql_precondition_node_kind kind) {
    return kind >= QL_PRECONDITION_NODE_UNSIGNED_ADD &&
           kind <= QL_PRECONDITION_NODE_UNSIGNED_MULTIPLY;
}

static ql_status parse_binary(ql_parse_context *context, yyjson_val *value,
                              uint32_t depth,
                              ql_precondition_node_kind kind,
                              uint32_t *result) {
    static const char *const fields[] = {"op", "left", "right"};
    ql_precondition_node_record node;
    uint32_t children[2];
    const ql_precondition_node_record *left;
    const ql_precondition_node_record *right;
    ql_status status = require_object_fields(
        value, "binary expression", fields,
        sizeof(fields) / sizeof(fields[0]), context->error);
    if (status != QL_STATUS_OK) {
        return status;
    }
    status = parse_expression(context, yyjson_obj_get(value, "left"),
                              depth + 1u, &children[0]);
    if (status != QL_STATUS_OK) {
        return status;
    }
    status = parse_expression(context, yyjson_obj_get(value, "right"),
                              depth + 1u, &children[1]);
    if (status != QL_STATUS_OK) {
        return status;
    }
    left = &context->precondition->nodes[children[0]];
    right = &context->precondition->nodes[children[1]];
    if (!value_types_equal(left, right)) {
        ql_error_set(context->error, QL_STATUS_TYPE_MISMATCH,
                     "binary operands must have identical types and widths");
        return QL_STATUS_TYPE_MISMATCH;
    }
    if (kind == QL_PRECONDITION_NODE_IMPLIES) {
        if (left->value_kind != QL_PRECONDITION_VALUE_BOOL) {
            ql_error_set(context->error, QL_STATUS_TYPE_MISMATCH,
                         "implies operands must be boolean");
            return QL_STATUS_TYPE_MISMATCH;
        }
    } else if (kind == QL_PRECONDITION_NODE_EQUAL ||
               kind == QL_PRECONDITION_NODE_NOT_EQUAL) {
        if (left->value_kind == QL_PRECONDITION_VALUE_RANGE ||
            left->value_kind == QL_PRECONDITION_VALUE_INVALID) {
            ql_error_set(context->error, QL_STATUS_TYPE_MISMATCH,
                         "eq/ne do not accept range operands");
            return QL_STATUS_TYPE_MISMATCH;
        }
    } else if (kind_is_signed_comparison(kind) != 0 ||
               kind_is_signed_arithmetic(kind) != 0) {
        if (left->value_kind != QL_PRECONDITION_VALUE_SIGNED_INTEGER) {
            ql_error_set(context->error, QL_STATUS_TYPE_MISMATCH,
                         "signed operator requires equal-width signed integers");
            return QL_STATUS_TYPE_MISMATCH;
        }
    } else if (kind_is_unsigned_comparison(kind) != 0 ||
               kind_is_unsigned_arithmetic(kind) != 0) {
        if (left->value_kind != QL_PRECONDITION_VALUE_UNSIGNED_INTEGER) {
            ql_error_set(context->error, QL_STATUS_TYPE_MISMATCH,
                         "unsigned operator requires equal-width unsigned integers");
            return QL_STATUS_TYPE_MISMATCH;
        }
    }
    memset(&node, 0, sizeof(node));
    node.kind = kind;
    node.value_kind =
        kind_is_signed_arithmetic(kind) != 0
            ? QL_PRECONDITION_VALUE_SIGNED_INTEGER
            : (kind_is_unsigned_arithmetic(kind) != 0
                   ? QL_PRECONDITION_VALUE_UNSIGNED_INTEGER
                   : QL_PRECONDITION_VALUE_BOOL);
    node.bit_width = node.value_kind == QL_PRECONDITION_VALUE_BOOL
                         ? 1u
                         : left->bit_width;
    node.child_count = 2u;
    status = copy_children(context, children, 2u, &node.children);
    if (status != QL_STATUS_OK) {
        return status;
    }
    if (kind == QL_PRECONDITION_NODE_IMPLIES && left->truth_known != 0u &&
        right->truth_known != 0u) {
        node.truth_known = 1u;
        node.truth_value = left->truth_value == 0u || right->truth_value != 0u;
    } else {
        set_binary_truth(context->precondition, &node);
    }
    status = add_node(context, &node, result);
    if (status != QL_STATUS_OK) {
        context->precondition->allocator.deallocate(
            context->precondition->allocator.user_data, node.children);
    }
    return status;
}

static int node_is_zero_integer(const ql_precondition_node_record *node) {
    return node->kind == QL_PRECONDITION_NODE_INTEGER &&
           node->integer_value_size == 1u && node->integer_value[0] == '0';
}

static ql_status parse_range(ql_parse_context *context, yyjson_val *value,
                             uint32_t depth, uint32_t *result) {
    static const char *const fields[] = {"pointer", "offset", "bytes"};
    ql_precondition_node_record node;
    uint32_t children[3];
    const ql_precondition_node_record *pointer;
    const ql_precondition_node_record *offset;
    const ql_precondition_node_record *bytes;
    ql_status status = require_object_fields(
        value, "range", fields, sizeof(fields) / sizeof(fields[0]),
        context->error);
    if (status != QL_STATUS_OK) {
        return status;
    }
    status = parse_expression(context, yyjson_obj_get(value, "pointer"),
                              depth + 1u, &children[0]);
    if (status != QL_STATUS_OK) {
        return status;
    }
    status = parse_expression(context, yyjson_obj_get(value, "offset"),
                              depth + 1u, &children[1]);
    if (status != QL_STATUS_OK) {
        return status;
    }
    status = parse_expression(context, yyjson_obj_get(value, "bytes"),
                              depth + 1u, &children[2]);
    if (status != QL_STATUS_OK) {
        return status;
    }
    pointer = &context->precondition->nodes[children[0]];
    offset = &context->precondition->nodes[children[1]];
    bytes = &context->precondition->nodes[children[2]];
    if (pointer->value_kind != QL_PRECONDITION_VALUE_POINTER ||
        offset->value_kind != QL_PRECONDITION_VALUE_SIGNED_INTEGER ||
        bytes->value_kind != QL_PRECONDITION_VALUE_UNSIGNED_INTEGER ||
        offset->bit_width != pointer->bit_width ||
        bytes->bit_width != pointer->bit_width) {
        ql_error_set(context->error, QL_STATUS_TYPE_MISMATCH,
                     "range requires a pointer plus pointer-width signed offset and unsigned byte count");
        return QL_STATUS_TYPE_MISMATCH;
    }
    if (node_is_zero_integer(bytes)) {
        ql_error_set(context->error, QL_STATUS_INVALID_ARGUMENT,
                     "range byte count cannot be the constant zero");
        return QL_STATUS_INVALID_ARGUMENT;
    }
    memset(&node, 0, sizeof(node));
    node.kind = QL_PRECONDITION_NODE_RANGE;
    node.value_kind = QL_PRECONDITION_VALUE_RANGE;
    node.bit_width = pointer->bit_width;
    node.address_space = pointer->address_space;
    node.child_count = 3u;
    status = copy_children(context, children, 3u, &node.children);
    if (status != QL_STATUS_OK) {
        return status;
    }
    status = add_node(context, &node, result);
    if (status != QL_STATUS_OK) {
        context->precondition->allocator.deallocate(
            context->precondition->allocator.user_data, node.children);
    }
    return status;
}

static ql_status parse_valid_range(ql_parse_context *context,
                                   yyjson_val *value, uint32_t depth,
                                   uint32_t *result) {
    static const char *const fields[] = {
        "op",       "range",    "read",       "write",
        "alignment", "nullable", "alias_group"
    };
    ql_precondition_node_record node;
    yyjson_val *read_value;
    yyjson_val *write_value;
    yyjson_val *alignment_value;
    yyjson_val *nullable_value;
    yyjson_val *alias_group_value;
    uint32_t range;
    uint32_t alias_group;
    uint64_t alignment;
    ql_status status = require_object_fields(
        value, "valid_range expression", fields,
        sizeof(fields) / sizeof(fields[0]), context->error);
    if (status != QL_STATUS_OK) {
        return status;
    }
    status = parse_range(context, yyjson_obj_get(value, "range"), depth + 1u,
                         &range);
    if (status != QL_STATUS_OK) {
        return status;
    }
    read_value = yyjson_obj_get(value, "read");
    write_value = yyjson_obj_get(value, "write");
    alignment_value = yyjson_obj_get(value, "alignment");
    nullable_value = yyjson_obj_get(value, "nullable");
    alias_group_value = yyjson_obj_get(value, "alias_group");
    if (!yyjson_is_bool(read_value) || !yyjson_is_bool(write_value) ||
        !yyjson_is_uint(alignment_value) ||
        !yyjson_is_bool(nullable_value) ||
        !get_uint32_value(alias_group_value, &alias_group)) {
        ql_error_set(context->error, QL_STATUS_TYPE_MISMATCH,
                     "valid_range properties have invalid types");
        return QL_STATUS_TYPE_MISMATCH;
    }
    alignment = yyjson_get_uint(alignment_value);
    if ((!yyjson_get_bool(read_value) && !yyjson_get_bool(write_value)) ||
        !is_power_of_two(alignment) ||
        !alignment_fits_width(
            alignment, context->precondition->nodes[range].bit_width)) {
        ql_error_set(context->error, QL_STATUS_INVALID_ARGUMENT,
                     "valid_range needs read or write and a fitting power-of-two alignment");
        return QL_STATUS_INVALID_ARGUMENT;
    }
    memset(&node, 0, sizeof(node));
    node.kind = QL_PRECONDITION_NODE_VALID_RANGE;
    node.value_kind = QL_PRECONDITION_VALUE_BOOL;
    node.bit_width = 1u;
    node.child_count = 1u;
    node.access = (yyjson_get_bool(read_value)
                       ? QL_PRECONDITION_ACCESS_READ
                       : 0u) |
                  (yyjson_get_bool(write_value)
                       ? QL_PRECONDITION_ACCESS_WRITE
                       : 0u);
    node.alignment = alignment;
    node.nullable = yyjson_get_bool(nullable_value) ? 1u : 0u;
    node.alias_group = alias_group;
    status = copy_children(context, &range, 1u, &node.children);
    if (status != QL_STATUS_OK) {
        return status;
    }
    status = add_node(context, &node, result);
    if (status != QL_STATUS_OK) {
        context->precondition->allocator.deallocate(
            context->precondition->allocator.user_data, node.children);
    }
    return status;
}

static ql_status parse_aligned(ql_parse_context *context, yyjson_val *value,
                               uint32_t depth, uint32_t *result) {
    static const char *const fields[] = {"op", "pointer", "offset",
                                         "alignment"};
    ql_precondition_node_record node;
    uint32_t children[2];
    const ql_precondition_node_record *pointer;
    const ql_precondition_node_record *offset;
    yyjson_val *alignment_value;
    uint64_t alignment;
    ql_status status = require_object_fields(
        value, "aligned expression", fields,
        sizeof(fields) / sizeof(fields[0]), context->error);
    if (status != QL_STATUS_OK) {
        return status;
    }
    status = parse_expression(context, yyjson_obj_get(value, "pointer"),
                              depth + 1u, &children[0]);
    if (status != QL_STATUS_OK) {
        return status;
    }
    status = parse_expression(context, yyjson_obj_get(value, "offset"),
                              depth + 1u, &children[1]);
    if (status != QL_STATUS_OK) {
        return status;
    }
    pointer = &context->precondition->nodes[children[0]];
    offset = &context->precondition->nodes[children[1]];
    alignment_value = yyjson_obj_get(value, "alignment");
    if (pointer->value_kind != QL_PRECONDITION_VALUE_POINTER ||
        offset->value_kind != QL_PRECONDITION_VALUE_SIGNED_INTEGER ||
        offset->bit_width != pointer->bit_width ||
        !yyjson_is_uint(alignment_value)) {
        ql_error_set(context->error, QL_STATUS_TYPE_MISMATCH,
                     "aligned requires a pointer and matching signed offset");
        return QL_STATUS_TYPE_MISMATCH;
    }
    alignment = yyjson_get_uint(alignment_value);
    if (!is_power_of_two(alignment) ||
        !alignment_fits_width(alignment, pointer->bit_width)) {
        ql_error_set(context->error, QL_STATUS_INVALID_ARGUMENT,
                     "aligned alignment must be a fitting nonzero power of two");
        return QL_STATUS_INVALID_ARGUMENT;
    }
    memset(&node, 0, sizeof(node));
    node.kind = QL_PRECONDITION_NODE_ALIGNED;
    node.value_kind = QL_PRECONDITION_VALUE_BOOL;
    node.bit_width = 1u;
    node.child_count = 2u;
    node.alignment = alignment;
    status = copy_children(context, children, 2u, &node.children);
    if (status != QL_STATUS_OK) {
        return status;
    }
    status = add_node(context, &node, result);
    if (status != QL_STATUS_OK) {
        context->precondition->allocator.deallocate(
            context->precondition->allocator.user_data, node.children);
    }
    return status;
}

static ql_status parse_disjoint(ql_parse_context *context, yyjson_val *value,
                                uint32_t depth, uint32_t *result) {
    static const char *const fields[] = {"op", "left", "right"};
    ql_precondition_node_record node;
    uint32_t children[2];
    const ql_precondition_node_record *left;
    const ql_precondition_node_record *right;
    ql_status status = require_object_fields(
        value, "disjoint expression", fields,
        sizeof(fields) / sizeof(fields[0]), context->error);
    if (status != QL_STATUS_OK) {
        return status;
    }
    status = parse_range(context, yyjson_obj_get(value, "left"), depth + 1u,
                         &children[0]);
    if (status != QL_STATUS_OK) {
        return status;
    }
    status = parse_range(context, yyjson_obj_get(value, "right"), depth + 1u,
                         &children[1]);
    if (status != QL_STATUS_OK) {
        return status;
    }
    left = &context->precondition->nodes[children[0]];
    right = &context->precondition->nodes[children[1]];
    if (left->bit_width != right->bit_width ||
        left->address_space != right->address_space) {
        ql_error_set(context->error, QL_STATUS_TYPE_MISMATCH,
                     "disjoint ranges must use the same pointer width and address space");
        return QL_STATUS_TYPE_MISMATCH;
    }
    memset(&node, 0, sizeof(node));
    node.kind = QL_PRECONDITION_NODE_DISJOINT;
    node.value_kind = QL_PRECONDITION_VALUE_BOOL;
    node.bit_width = 1u;
    node.child_count = 2u;
    status = copy_children(context, children, 2u, &node.children);
    if (status != QL_STATUS_OK) {
        return status;
    }
    if (nodes_equal(context->precondition, children[0], children[1], 0u)) {
        node.truth_known = 1u;
        node.truth_value = 0u;
    }
    status = add_node(context, &node, result);
    if (status != QL_STATUS_OK) {
        context->precondition->allocator.deallocate(
            context->precondition->allocator.user_data, node.children);
    }
    return status;
}

static ql_status parse_expression(ql_parse_context *context,
                                  yyjson_val *value, uint32_t depth,
                                  uint32_t *result) {
    yyjson_val *operation;

    if (depth > QL_PRECONDITION_MAX_DEPTH) {
        ql_error_set(context->error, QL_STATUS_PARSE_ERROR,
                     "precondition nesting exceeds %u levels",
                     QL_PRECONDITION_MAX_DEPTH);
        return QL_STATUS_PARSE_ERROR;
    }
    if (yyjson_is_bool(value)) {
        return parse_boolean_literal(context, value, result);
    }
    if (!yyjson_is_obj(value)) {
        ql_error_set(context->error, QL_STATUS_SCHEMA_MISMATCH,
                     "precondition expression must be a boolean or object");
        return QL_STATUS_SCHEMA_MISMATCH;
    }
    operation = yyjson_obj_get(value, "op");
    if (!yyjson_is_str(operation)) {
        ql_error_set(context->error, QL_STATUS_SCHEMA_MISMATCH,
                     "precondition expression has no string op field");
        return QL_STATUS_SCHEMA_MISMATCH;
    }
    if (json_string_equals(operation, "arg")) {
        return parse_argument(context, value, result);
    }
    if (json_string_equals(operation, "int")) {
        return parse_integer(context, value, result);
    }
    if (json_string_equals(operation, "not")) {
        return parse_not(context, value, depth, result);
    }
    if (json_string_equals(operation, "and")) {
        return parse_nary_boolean(context, value, depth,
                                  QL_PRECONDITION_NODE_AND, result);
    }
    if (json_string_equals(operation, "or")) {
        return parse_nary_boolean(context, value, depth,
                                  QL_PRECONDITION_NODE_OR, result);
    }
    if (json_string_equals(operation, "implies")) {
        return parse_binary(context, value, depth,
                            QL_PRECONDITION_NODE_IMPLIES, result);
    }
    if (json_string_equals(operation, "eq")) {
        return parse_binary(context, value, depth,
                            QL_PRECONDITION_NODE_EQUAL, result);
    }
    if (json_string_equals(operation, "ne")) {
        return parse_binary(context, value, depth,
                            QL_PRECONDITION_NODE_NOT_EQUAL, result);
    }
    if (json_string_equals(operation, "slt")) {
        return parse_binary(context, value, depth,
                            QL_PRECONDITION_NODE_SIGNED_LESS, result);
    }
    if (json_string_equals(operation, "sle")) {
        return parse_binary(context, value, depth,
                            QL_PRECONDITION_NODE_SIGNED_LESS_EQUAL, result);
    }
    if (json_string_equals(operation, "sgt")) {
        return parse_binary(context, value, depth,
                            QL_PRECONDITION_NODE_SIGNED_GREATER, result);
    }
    if (json_string_equals(operation, "sge")) {
        return parse_binary(context, value, depth,
                            QL_PRECONDITION_NODE_SIGNED_GREATER_EQUAL, result);
    }
    if (json_string_equals(operation, "ult")) {
        return parse_binary(context, value, depth,
                            QL_PRECONDITION_NODE_UNSIGNED_LESS, result);
    }
    if (json_string_equals(operation, "ule")) {
        return parse_binary(context, value, depth,
                            QL_PRECONDITION_NODE_UNSIGNED_LESS_EQUAL, result);
    }
    if (json_string_equals(operation, "ugt")) {
        return parse_binary(context, value, depth,
                            QL_PRECONDITION_NODE_UNSIGNED_GREATER, result);
    }
    if (json_string_equals(operation, "uge")) {
        return parse_binary(context, value, depth,
                            QL_PRECONDITION_NODE_UNSIGNED_GREATER_EQUAL,
                            result);
    }
    if (json_string_equals(operation, "sadd")) {
        return parse_binary(context, value, depth,
                            QL_PRECONDITION_NODE_SIGNED_ADD, result);
    }
    if (json_string_equals(operation, "ssub")) {
        return parse_binary(context, value, depth,
                            QL_PRECONDITION_NODE_SIGNED_SUBTRACT, result);
    }
    if (json_string_equals(operation, "smul")) {
        return parse_binary(context, value, depth,
                            QL_PRECONDITION_NODE_SIGNED_MULTIPLY, result);
    }
    if (json_string_equals(operation, "uadd")) {
        return parse_binary(context, value, depth,
                            QL_PRECONDITION_NODE_UNSIGNED_ADD, result);
    }
    if (json_string_equals(operation, "usub")) {
        return parse_binary(context, value, depth,
                            QL_PRECONDITION_NODE_UNSIGNED_SUBTRACT, result);
    }
    if (json_string_equals(operation, "umul")) {
        return parse_binary(context, value, depth,
                            QL_PRECONDITION_NODE_UNSIGNED_MULTIPLY, result);
    }
    if (json_string_equals(operation, "valid_range")) {
        return parse_valid_range(context, value, depth, result);
    }
    if (json_string_equals(operation, "aligned")) {
        return parse_aligned(context, value, depth, result);
    }
    if (json_string_equals(operation, "disjoint")) {
        return parse_disjoint(context, value, depth, result);
    }
    ql_error_set(context->error, QL_STATUS_SCHEMA_MISMATCH,
                 "precondition expression uses an unknown operator");
    return QL_STATUS_SCHEMA_MISMATCH;
}

static ql_status buffer_reserve(ql_text_buffer *buffer, size_t additional,
                                ql_error *error) {
    size_t required;
    size_t capacity;
    void *resized;

    if (additional > SIZE_MAX - buffer->size - 1u) {
        ql_error_set(error, QL_STATUS_OUT_OF_MEMORY, NULL);
        return QL_STATUS_OUT_OF_MEMORY;
    }
    required = buffer->size + additional + 1u;
    if (required <= buffer->capacity) {
        return QL_STATUS_OK;
    }
    capacity = buffer->capacity != 0u ? buffer->capacity : 256u;
    while (capacity < required) {
        if (capacity > SIZE_MAX / 2u) {
            capacity = required;
            break;
        }
        capacity *= 2u;
    }
    resized = buffer->allocator->reallocate(buffer->allocator->user_data,
                                            buffer->data, capacity);
    if (resized == NULL) {
        ql_error_set(error, QL_STATUS_OUT_OF_MEMORY, NULL);
        return QL_STATUS_OUT_OF_MEMORY;
    }
    buffer->data = (char *)resized;
    buffer->capacity = capacity;
    return QL_STATUS_OK;
}

static ql_status buffer_append_bytes(ql_text_buffer *buffer,
                                     const void *bytes, size_t size,
                                     ql_error *error) {
    ql_status status = buffer_reserve(buffer, size, error);
    if (status != QL_STATUS_OK) {
        return status;
    }
    if (size != 0u) {
        memcpy(buffer->data + buffer->size, bytes, size);
        buffer->size += size;
    }
    buffer->data[buffer->size] = '\0';
    return QL_STATUS_OK;
}

static ql_status buffer_append_text(ql_text_buffer *buffer, const char *text,
                                    ql_error *error) {
    return buffer_append_bytes(buffer, text, strlen(text), error);
}

static ql_status buffer_append_uint64(ql_text_buffer *buffer, uint64_t value,
                                      ql_error *error) {
    char text[32];
    int length = snprintf(text, sizeof(text), "%llu",
                          (unsigned long long)value);
    if (length < 0 || (size_t)length >= sizeof(text)) {
        ql_error_set(error, QL_STATUS_INTERNAL_ERROR,
                     "cannot format canonical unsigned integer");
        return QL_STATUS_INTERNAL_ERROR;
    }
    return buffer_append_bytes(buffer, text, (size_t)length, error);
}

static const char *node_operator_name(ql_precondition_node_kind kind) {
    switch (kind) {
    case QL_PRECONDITION_NODE_NOT: return "not";
    case QL_PRECONDITION_NODE_AND: return "and";
    case QL_PRECONDITION_NODE_OR: return "or";
    case QL_PRECONDITION_NODE_IMPLIES: return "implies";
    case QL_PRECONDITION_NODE_EQUAL: return "eq";
    case QL_PRECONDITION_NODE_NOT_EQUAL: return "ne";
    case QL_PRECONDITION_NODE_SIGNED_LESS: return "slt";
    case QL_PRECONDITION_NODE_SIGNED_LESS_EQUAL: return "sle";
    case QL_PRECONDITION_NODE_SIGNED_GREATER: return "sgt";
    case QL_PRECONDITION_NODE_SIGNED_GREATER_EQUAL: return "sge";
    case QL_PRECONDITION_NODE_UNSIGNED_LESS: return "ult";
    case QL_PRECONDITION_NODE_UNSIGNED_LESS_EQUAL: return "ule";
    case QL_PRECONDITION_NODE_UNSIGNED_GREATER: return "ugt";
    case QL_PRECONDITION_NODE_UNSIGNED_GREATER_EQUAL: return "uge";
    case QL_PRECONDITION_NODE_SIGNED_ADD: return "sadd";
    case QL_PRECONDITION_NODE_SIGNED_SUBTRACT: return "ssub";
    case QL_PRECONDITION_NODE_SIGNED_MULTIPLY: return "smul";
    case QL_PRECONDITION_NODE_UNSIGNED_ADD: return "uadd";
    case QL_PRECONDITION_NODE_UNSIGNED_SUBTRACT: return "usub";
    case QL_PRECONDITION_NODE_UNSIGNED_MULTIPLY: return "umul";
    case QL_PRECONDITION_NODE_VALID_RANGE: return "valid_range";
    case QL_PRECONDITION_NODE_ALIGNED: return "aligned";
    case QL_PRECONDITION_NODE_DISJOINT: return "disjoint";
    default: return NULL;
    }
}

static ql_status write_node(const ql_precondition *precondition,
                            uint32_t node_index, ql_text_buffer *buffer,
                            ql_error *error);

static ql_status write_range_node(const ql_precondition *precondition,
                                  const ql_precondition_node_record *node,
                                  ql_text_buffer *buffer, ql_error *error) {
    ql_status status;
    status = buffer_append_text(buffer, "{\"pointer\":", error);
    if (status != QL_STATUS_OK) return status;
    status = write_node(precondition, node->children[0], buffer, error);
    if (status != QL_STATUS_OK) return status;
    status = buffer_append_text(buffer, ",\"offset\":", error);
    if (status != QL_STATUS_OK) return status;
    status = write_node(precondition, node->children[1], buffer, error);
    if (status != QL_STATUS_OK) return status;
    status = buffer_append_text(buffer, ",\"bytes\":", error);
    if (status != QL_STATUS_OK) return status;
    status = write_node(precondition, node->children[2], buffer, error);
    if (status != QL_STATUS_OK) return status;
    return buffer_append_text(buffer, "}", error);
}

static int kind_is_binary(ql_precondition_node_kind kind) {
    return kind == QL_PRECONDITION_NODE_IMPLIES ||
           kind == QL_PRECONDITION_NODE_EQUAL ||
           kind == QL_PRECONDITION_NODE_NOT_EQUAL ||
           kind_is_signed_comparison(kind) != 0 ||
           kind_is_unsigned_comparison(kind) != 0 ||
           kind_is_signed_arithmetic(kind) != 0 ||
           kind_is_unsigned_arithmetic(kind) != 0;
}

static ql_status write_node(const ql_precondition *precondition,
                            uint32_t node_index, ql_text_buffer *buffer,
                            ql_error *error) {
    const ql_precondition_node_record *node;
    const char *operation;
    ql_status status;
    size_t child_index;

    if (node_index >= precondition->node_count) {
        ql_error_set(error, QL_STATUS_INTERNAL_ERROR,
                     "precondition AST contains an invalid child index");
        return QL_STATUS_INTERNAL_ERROR;
    }
    node = &precondition->nodes[node_index];
    switch (node->kind) {
    case QL_PRECONDITION_NODE_BOOL:
        return buffer_append_text(buffer,
                                  node->boolean_value != 0u ? "true"
                                                            : "false",
                                  error);
    case QL_PRECONDITION_NODE_ARGUMENT:
        status = buffer_append_text(buffer, "{\"op\":\"arg\",\"index\":",
                                    error);
        if (status != QL_STATUS_OK) return status;
        status = buffer_append_uint64(buffer, node->argument_index, error);
        if (status != QL_STATUS_OK) return status;
        return buffer_append_text(buffer, "}", error);
    case QL_PRECONDITION_NODE_INTEGER:
        status = buffer_append_text(buffer,
                                    "{\"op\":\"int\",\"signed\":", error);
        if (status != QL_STATUS_OK) return status;
        status = buffer_append_text(
            buffer,
            node->value_kind == QL_PRECONDITION_VALUE_SIGNED_INTEGER
                ? "true"
                : "false",
            error);
        if (status != QL_STATUS_OK) return status;
        status = buffer_append_text(buffer, ",\"width\":", error);
        if (status != QL_STATUS_OK) return status;
        status = buffer_append_uint64(buffer, node->bit_width, error);
        if (status != QL_STATUS_OK) return status;
        status = buffer_append_text(buffer, ",\"value\":\"", error);
        if (status != QL_STATUS_OK) return status;
        status = buffer_append_bytes(buffer, node->integer_value,
                                     node->integer_value_size, error);
        if (status != QL_STATUS_OK) return status;
        return buffer_append_text(buffer, "\"}", error);
    case QL_PRECONDITION_NODE_NOT:
        status = buffer_append_text(buffer, "{\"op\":\"not\",\"value\":",
                                    error);
        if (status != QL_STATUS_OK) return status;
        status = write_node(precondition, node->children[0], buffer, error);
        if (status != QL_STATUS_OK) return status;
        return buffer_append_text(buffer, "}", error);
    case QL_PRECONDITION_NODE_AND:
    case QL_PRECONDITION_NODE_OR:
        status = buffer_append_text(
            buffer,
            node->kind == QL_PRECONDITION_NODE_AND
                ? "{\"op\":\"and\",\"args\":["
                : "{\"op\":\"or\",\"args\":[",
            error);
        if (status != QL_STATUS_OK) return status;
        for (child_index = 0u; child_index < node->child_count;
             ++child_index) {
            if (child_index != 0u) {
                status = buffer_append_text(buffer, ",", error);
                if (status != QL_STATUS_OK) return status;
            }
            status = write_node(precondition, node->children[child_index],
                                buffer, error);
            if (status != QL_STATUS_OK) return status;
        }
        return buffer_append_text(buffer, "]}", error);
    case QL_PRECONDITION_NODE_RANGE:
        return write_range_node(precondition, node, buffer, error);
    case QL_PRECONDITION_NODE_VALID_RANGE:
        status = buffer_append_text(
            buffer, "{\"op\":\"valid_range\",\"range\":", error);
        if (status != QL_STATUS_OK) return status;
        status = write_node(precondition, node->children[0], buffer, error);
        if (status != QL_STATUS_OK) return status;
        status = buffer_append_text(buffer, ",\"read\":", error);
        if (status != QL_STATUS_OK) return status;
        status = buffer_append_text(
            buffer,
            (node->access & QL_PRECONDITION_ACCESS_READ) != 0u ? "true"
                                                               : "false",
            error);
        if (status != QL_STATUS_OK) return status;
        status = buffer_append_text(buffer, ",\"write\":", error);
        if (status != QL_STATUS_OK) return status;
        status = buffer_append_text(
            buffer,
            (node->access & QL_PRECONDITION_ACCESS_WRITE) != 0u ? "true"
                                                                : "false",
            error);
        if (status != QL_STATUS_OK) return status;
        status = buffer_append_text(buffer, ",\"alignment\":", error);
        if (status != QL_STATUS_OK) return status;
        status = buffer_append_uint64(buffer, node->alignment, error);
        if (status != QL_STATUS_OK) return status;
        status = buffer_append_text(buffer, ",\"nullable\":", error);
        if (status != QL_STATUS_OK) return status;
        status = buffer_append_text(buffer,
                                    node->nullable != 0u ? "true" : "false",
                                    error);
        if (status != QL_STATUS_OK) return status;
        status = buffer_append_text(buffer, ",\"alias_group\":", error);
        if (status != QL_STATUS_OK) return status;
        status = buffer_append_uint64(buffer, node->alias_group, error);
        if (status != QL_STATUS_OK) return status;
        return buffer_append_text(buffer, "}", error);
    case QL_PRECONDITION_NODE_ALIGNED:
        status = buffer_append_text(buffer,
                                    "{\"op\":\"aligned\",\"pointer\":",
                                    error);
        if (status != QL_STATUS_OK) return status;
        status = write_node(precondition, node->children[0], buffer, error);
        if (status != QL_STATUS_OK) return status;
        status = buffer_append_text(buffer, ",\"offset\":", error);
        if (status != QL_STATUS_OK) return status;
        status = write_node(precondition, node->children[1], buffer, error);
        if (status != QL_STATUS_OK) return status;
        status = buffer_append_text(buffer, ",\"alignment\":", error);
        if (status != QL_STATUS_OK) return status;
        status = buffer_append_uint64(buffer, node->alignment, error);
        if (status != QL_STATUS_OK) return status;
        return buffer_append_text(buffer, "}", error);
    case QL_PRECONDITION_NODE_DISJOINT:
        status = buffer_append_text(buffer,
                                    "{\"op\":\"disjoint\",\"left\":",
                                    error);
        if (status != QL_STATUS_OK) return status;
        status = write_node(precondition, node->children[0], buffer, error);
        if (status != QL_STATUS_OK) return status;
        status = buffer_append_text(buffer, ",\"right\":", error);
        if (status != QL_STATUS_OK) return status;
        status = write_node(precondition, node->children[1], buffer, error);
        if (status != QL_STATUS_OK) return status;
        return buffer_append_text(buffer, "}", error);
    default:
        break;
    }
    if (!kind_is_binary(node->kind)) {
        ql_error_set(error, QL_STATUS_INTERNAL_ERROR,
                     "precondition AST contains an invalid node kind");
        return QL_STATUS_INTERNAL_ERROR;
    }
    operation = node_operator_name(node->kind);
    if (operation == NULL) {
        ql_error_set(error, QL_STATUS_INTERNAL_ERROR,
                     "precondition binary node has no operator name");
        return QL_STATUS_INTERNAL_ERROR;
    }
    status = buffer_append_text(buffer, "{\"op\":\"", error);
    if (status != QL_STATUS_OK) return status;
    status = buffer_append_text(buffer, operation, error);
    if (status != QL_STATUS_OK) return status;
    status = buffer_append_text(buffer, "\",\"left\":", error);
    if (status != QL_STATUS_OK) return status;
    status = write_node(precondition, node->children[0], buffer, error);
    if (status != QL_STATUS_OK) return status;
    status = buffer_append_text(buffer, ",\"right\":", error);
    if (status != QL_STATUS_OK) return status;
    status = write_node(precondition, node->children[1], buffer, error);
    if (status != QL_STATUS_OK) return status;
    return buffer_append_text(buffer, "}", error);
}

static ql_status write_signature(const ql_signature_view_v1 *signature,
                                 ql_text_buffer *buffer, ql_error *error) {
    size_t index;
    ql_status status = buffer_append_text(
        buffer, "{\"schema_version\":1,\"pointer_width\":", error);
    if (status != QL_STATUS_OK) return status;
    status = buffer_append_uint64(buffer, signature->pointer_width, error);
    if (status != QL_STATUS_OK) return status;
    status = buffer_append_text(buffer, ",\"arguments\":[", error);
    if (status != QL_STATUS_OK) return status;
    for (index = 0u; index < signature->argument_count; ++index) {
        const ql_signature_argument_v1 *argument =
            &signature->arguments[index];
        if (index != 0u) {
            status = buffer_append_text(buffer, ",", error);
            if (status != QL_STATUS_OK) return status;
        }
        status = buffer_append_text(buffer, "{\"kind\":", error);
        if (status != QL_STATUS_OK) return status;
        status = buffer_append_uint64(buffer, (uint64_t)argument->kind, error);
        if (status != QL_STATUS_OK) return status;
        status = buffer_append_text(buffer, ",\"width\":", error);
        if (status != QL_STATUS_OK) return status;
        status = buffer_append_uint64(buffer, argument->bit_width, error);
        if (status != QL_STATUS_OK) return status;
        status = buffer_append_text(buffer, ",\"address_space\":", error);
        if (status != QL_STATUS_OK) return status;
        status = buffer_append_uint64(buffer, argument->address_space, error);
        if (status != QL_STATUS_OK) return status;
        status = buffer_append_text(buffer, "}", error);
        if (status != QL_STATUS_OK) return status;
    }
    return buffer_append_text(buffer, "]}", error);
}

void QL_CALL ql_precondition_destroy(ql_precondition *precondition) {
    size_t index;
    if (precondition == NULL) {
        return;
    }
    for (index = 0u; index < precondition->node_count; ++index) {
        precondition->allocator.deallocate(
            precondition->allocator.user_data,
            precondition->nodes[index].children);
        precondition->allocator.deallocate(
            precondition->allocator.user_data,
            precondition->nodes[index].integer_value);
    }
    precondition->allocator.deallocate(precondition->allocator.user_data,
                                       precondition->canonical_bytes);
    precondition->allocator.deallocate(precondition->allocator.user_data,
                                       precondition->nodes);
    precondition->allocator.deallocate(precondition->allocator.user_data,
                                       precondition);
}

ql_status QL_CALL ql_precondition_parse(
    const ql_allocator *allocator, const char *json, size_t json_size,
    const ql_signature_view_v1 *signature, ql_precondition **output,
    ql_error *error) {
    static const char default_json[] =
        "{\"schema_version\":1,\"expression\":true}";
    static const char *const root_fields[] = {"schema_version", "expression"};
    static const char digest_domain[] = "quodlibet.precondition.v1";
    const ql_allocator *selected = select_allocator(allocator);
    ql_precondition *precondition = NULL;
    ql_parse_context context;
    yyjson_alc json_allocator;
    yyjson_read_err read_error;
    yyjson_doc *document = NULL;
    yyjson_val *root;
    yyjson_val *schema_version;
    yyjson_val *expression;
    ql_text_buffer canonical;
    ql_text_buffer signature_bytes;
    ql_text_buffer digest_bytes;
    ql_status status;

    memset(&canonical, 0, sizeof(canonical));
    memset(&signature_bytes, 0, sizeof(signature_bytes));
    memset(&digest_bytes, 0, sizeof(digest_bytes));
    if (output == NULL) {
        ql_error_set(error, QL_STATUS_INVALID_ARGUMENT,
                     "precondition output pointer is required");
        return QL_STATUS_INVALID_ARGUMENT;
    }
    *output = NULL;
    if (!ql_allocator_is_valid(selected)) {
        ql_error_set(error, QL_STATUS_INVALID_ARGUMENT,
                     "precondition allocator is invalid");
        return QL_STATUS_INVALID_ARGUMENT;
    }
    status = ql_signature_view_validate(signature, error);
    if (status != QL_STATUS_OK) {
        return status;
    }
    if ((json == NULL) != (json_size == 0u)) {
        ql_error_set(error, QL_STATUS_INVALID_ARGUMENT,
                     "precondition JSON pointer and size disagree");
        return QL_STATUS_INVALID_ARGUMENT;
    }
    if (json == NULL) {
        json = default_json;
        json_size = sizeof(default_json) - 1u;
    }
    if (yyjson_read_max_memory_usage(json_size, 0u) == 0u) {
        ql_error_set(error, QL_STATUS_INVALID_ARGUMENT,
                     "precondition JSON size overflows parser limits");
        return QL_STATUS_INVALID_ARGUMENT;
    }
    precondition = selected->allocate(selected->user_data,
                                      sizeof(*precondition));
    if (precondition == NULL) {
        ql_error_set(error, QL_STATUS_OUT_OF_MEMORY, NULL);
        return QL_STATUS_OUT_OF_MEMORY;
    }
    memset(precondition, 0, sizeof(*precondition));
    precondition->allocator = *selected;
    precondition->root_node = QL_PRECONDITION_NO_NODE;
    canonical.allocator = &precondition->allocator;
    signature_bytes.allocator = &precondition->allocator;
    digest_bytes.allocator = &precondition->allocator;
    json_allocator = make_json_allocator(&precondition->allocator);
    document = yyjson_read_opts((char *)(uintptr_t)json, json_size, 0u,
                                &json_allocator, &read_error);
    if (document == NULL) {
        status = read_error.code == YYJSON_READ_ERROR_MEMORY_ALLOCATION
                     ? QL_STATUS_OUT_OF_MEMORY
                     : QL_STATUS_PARSE_ERROR;
        ql_error_set(error, status, "invalid precondition JSON at byte %zu: %s",
                     read_error.pos,
                     read_error.msg != NULL ? read_error.msg : "parse error");
        goto fail;
    }
    root = yyjson_doc_get_root(document);
    status = require_object_fields(
        root, "precondition root", root_fields,
        sizeof(root_fields) / sizeof(root_fields[0]), error);
    if (status != QL_STATUS_OK) {
        goto fail;
    }
    schema_version = yyjson_obj_get(root, "schema_version");
    expression = yyjson_obj_get(root, "expression");
    if (!yyjson_is_uint(schema_version) ||
        yyjson_get_uint(schema_version) != QL_PRECONDITION_SCHEMA_VERSION) {
        status = QL_STATUS_SCHEMA_MISMATCH;
        ql_error_set(error, status,
                     "unsupported precondition schema version");
        goto fail;
    }
    context.precondition = precondition;
    context.signature = signature;
    context.error = error;
    status = parse_expression(&context, expression, 0u,
                              &precondition->root_node);
    if (status != QL_STATUS_OK) {
        goto fail;
    }
    if (precondition->nodes[precondition->root_node].value_kind !=
        QL_PRECONDITION_VALUE_BOOL) {
        status = QL_STATUS_TYPE_MISMATCH;
        ql_error_set(error, status,
                     "precondition root expression must be boolean");
        goto fail;
    }
    if (precondition->nodes[precondition->root_node].truth_known != 0u &&
        precondition->nodes[precondition->root_node].truth_value == 0u) {
        status = QL_STATUS_INVALID_ARGUMENT;
        ql_error_set(error, status,
                     "precondition has a statically empty input domain");
        goto fail;
    }
    status = buffer_append_text(
        &canonical, "{\"schema_version\":1,\"expression\":", error);
    if (status != QL_STATUS_OK) goto fail;
    status = write_node(precondition, precondition->root_node, &canonical,
                        error);
    if (status != QL_STATUS_OK) goto fail;
    status = buffer_append_text(&canonical, "}", error);
    if (status != QL_STATUS_OK) goto fail;

    status = write_signature(signature, &signature_bytes, error);
    if (status != QL_STATUS_OK) goto fail;
    ql_digest_data(signature_bytes.data, signature_bytes.size,
                   &precondition->signature_digest);
    status = buffer_append_bytes(&digest_bytes, digest_domain,
                                 sizeof(digest_domain) - 1u, error);
    if (status != QL_STATUS_OK) goto fail;
    status = buffer_append_bytes(&digest_bytes,
                                 precondition->signature_digest.bytes,
                                 QL_DIGEST_SIZE, error);
    if (status != QL_STATUS_OK) goto fail;
    status = buffer_append_bytes(&digest_bytes, canonical.data, canonical.size,
                                 error);
    if (status != QL_STATUS_OK) goto fail;
    ql_digest_data(digest_bytes.data, digest_bytes.size,
                   &precondition->digest);

    precondition->canonical_bytes = canonical.data;
    precondition->canonical_size = canonical.size;
    canonical.data = NULL;
    yyjson_doc_free(document);
    precondition->allocator.deallocate(precondition->allocator.user_data,
                                       signature_bytes.data);
    precondition->allocator.deallocate(precondition->allocator.user_data,
                                       digest_bytes.data);
    ql_error_clear(error);
    *output = precondition;
    return QL_STATUS_OK;

fail:
    if (document != NULL) {
        yyjson_doc_free(document);
    }
    if (precondition != NULL) {
        precondition->allocator.deallocate(precondition->allocator.user_data,
                                           canonical.data);
        precondition->allocator.deallocate(
            precondition->allocator.user_data, signature_bytes.data);
        precondition->allocator.deallocate(precondition->allocator.user_data,
                                           digest_bytes.data);
    }
    ql_precondition_destroy(precondition);
    return status;
}

ql_status QL_CALL ql_precondition_get_view(
    const ql_precondition *precondition, ql_precondition_view_v1 *view,
    ql_error *error) {
    if (precondition == NULL || view == NULL ||
        view->struct_size < sizeof(*view)) {
        ql_error_set(error, QL_STATUS_INVALID_ARGUMENT,
                     "precondition view v1 has an invalid size");
        return QL_STATUS_INVALID_ARGUMENT;
    }
    view->schema_version = QL_PRECONDITION_SCHEMA_VERSION;
    view->root_node = precondition->root_node;
    view->node_count = precondition->node_count;
    view->canonical_bytes = precondition->canonical_bytes;
    view->canonical_size = precondition->canonical_size;
    view->signature_digest = precondition->signature_digest;
    view->digest = precondition->digest;
    memset(view->reserved, 0, sizeof(view->reserved));
    ql_error_clear(error);
    return QL_STATUS_OK;
}

ql_status QL_CALL ql_precondition_node_at(
    const ql_precondition *precondition, size_t index,
    ql_precondition_node_view_v1 *view, ql_error *error) {
    const ql_precondition_node_record *node;
    if (precondition == NULL || view == NULL ||
        view->struct_size < sizeof(*view)) {
        ql_error_set(error, QL_STATUS_INVALID_ARGUMENT,
                     "precondition node view v1 has an invalid size");
        return QL_STATUS_INVALID_ARGUMENT;
    }
    if (index >= precondition->node_count) {
        ql_error_set(error, QL_STATUS_NOT_FOUND,
                     "precondition node index is out of range");
        return QL_STATUS_NOT_FOUND;
    }
    node = &precondition->nodes[index];
    view->index = (uint32_t)index;
    view->kind = node->kind;
    view->value_kind = node->value_kind;
    view->bit_width = node->bit_width;
    view->address_space = node->address_space;
    view->children = node->children;
    view->child_count = node->child_count;
    view->argument_index = node->argument_index;
    view->boolean_value = node->boolean_value;
    view->integer_value = node->integer_value;
    view->integer_value_size = node->integer_value_size;
    view->access = node->access;
    view->alignment = node->alignment;
    view->nullable = node->nullable;
    view->alias_group = node->alias_group;
    memset(view->reserved, 0, sizeof(view->reserved));
    ql_error_clear(error);
    return QL_STATUS_OK;
}
