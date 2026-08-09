#include "quodlibet/replay.h"

#include <stdio.h>
#include <string.h>

#include "quodlibet/precondition.h"
#include "quodlibet/signature.h"

#include "yyjson.h"

#define QL_REPLAY_MAX_PARAMETERS QL_SOURCE_SIGNATURE_MAX_ARGUMENTS
/* The concrete precondition evaluator works on the scalar slice's argument
   widths. A wider literal makes the replay inconclusive instead of guessed. */
#define QL_REPLAY_MAX_PRECONDITION_BITS 64u
/* An object's initial image has to be materialized byte for byte before the
   interpreter can run over it, and a model is free to pick an object gigabytes
   long. Past this bound the replay says it could not decide; it does not
   quietly substitute a smaller object, because that would be answering a
   different question than the one the model answered. The bounded violation
   query exists to get a second model that fits. */
#define QL_REPLAY_MAX_OBJECT_BYTES UINT64_C(4096)
#define QL_REPLAY_MAX_MEMORY_OVERRIDES 8192u

typedef struct replay_override {
    uint64_t address;
    uint8_t byte;
} replay_override;

/* The initial memory a model describes: one default byte plus the sparse
   stores layered on top of it. */
typedef struct replay_image {
    uint8_t fill;
    replay_override *overrides;
    size_t count;
    size_t capacity;
} replay_image;

typedef struct replay_object {
    uint64_t base;
    uint64_t model_size;
    uint64_t size;
    uint32_t left_base_parameter;
    uint32_t right_base_parameter;
    uint8_t *initial;
} replay_object;

struct ql_replay_witness {
    ql_allocator allocator;
    ql_replay_value_v1 *values;
    size_t count;
    replay_object *objects;
    size_t object_count;
    /* Zero when the model's memory could not be turned into a layout this
       replay can execute. The caller reports UNKNOWN rather than a verdict. */
    uint32_t layout_usable;
};

static const ql_allocator *select_allocator(const ql_allocator *allocator) {
    return allocator == NULL ? ql_default_allocator() : allocator;
}

/* --- S-expression scanning ------------------------------------------------ */

typedef struct replay_scanner {
    const char *text;
    size_t size;
    size_t cursor;
} replay_scanner;

static void scanner_skip_space(replay_scanner *scanner) {
    while (scanner->cursor < scanner->size) {
        const char ch = scanner->text[scanner->cursor];
        if (ch == ';') {
            while (scanner->cursor < scanner->size &&
                   scanner->text[scanner->cursor] != '\n') {
                ++scanner->cursor;
            }
            continue;
        }
        if (ch != ' ' && ch != '\t' && ch != '\r' && ch != '\n' &&
            ch != '\f' && ch != '\v') {
            return;
        }
        ++scanner->cursor;
    }
}

/* Reads one datum, an atom or a balanced list, and reports its extent. */
static int scanner_next(replay_scanner *scanner, const char **start,
                        size_t *length) {
    size_t depth = 0u;
    size_t begin;

    scanner_skip_space(scanner);
    if (scanner->cursor >= scanner->size) {
        return 0;
    }
    begin = scanner->cursor;
    if (scanner->text[scanner->cursor] != '(') {
        while (scanner->cursor < scanner->size) {
            const char ch = scanner->text[scanner->cursor];
            if (ch == '(' || ch == ')' || ch == ' ' || ch == '\t' ||
                ch == '\r' || ch == '\n' || ch == '\f' || ch == '\v') {
                break;
            }
            ++scanner->cursor;
        }
        if (scanner->cursor == begin) {
            /* A stray closing parenthesis. */
            ++scanner->cursor;
            *start = scanner->text + begin;
            *length = 1u;
            return 1;
        }
        *start = scanner->text + begin;
        *length = scanner->cursor - begin;
        return 1;
    }
    while (scanner->cursor < scanner->size) {
        const char ch = scanner->text[scanner->cursor++];
        if (ch == '(') {
            ++depth;
        } else if (ch == ')') {
            --depth;
            if (depth == 0u) {
                *start = scanner->text + begin;
                *length = scanner->cursor - begin;
                return 1;
            }
        }
    }
    return 0;
}

static int token_equals(const char *text, size_t size, const char *expected) {
    const size_t expected_size = strlen(expected);
    return size == expected_size && memcmp(text, expected, size) == 0;
}

/* --- Value decoding ------------------------------------------------------- */

static void bytes_set_bit(uint8_t *bytes, uint32_t position, int value) {
    if (value != 0) {
        bytes[position / 8u] =
            (uint8_t)(bytes[position / 8u] | (1u << (position % 8u)));
    }
}

static int decode_binary_literal(const char *text, size_t size,
                                 uint32_t width, uint8_t *bytes) {
    size_t index;

    if (size != (size_t)width) {
        return 0;
    }
    for (index = 0u; index < size; ++index) {
        const char ch = text[index];
        if (ch != '0' && ch != '1') {
            return 0;
        }
        bytes_set_bit(bytes, (uint32_t)(size - 1u - index), ch == '1');
    }
    return 1;
}

static int decode_hex_literal(const char *text, size_t size, uint32_t width,
                              uint8_t *bytes) {
    size_t index;

    if (size * 4u != (size_t)width) {
        return 0;
    }
    for (index = 0u; index < size; ++index) {
        const char ch = text[index];
        uint32_t nibble;
        uint32_t bit;
        if (ch >= '0' && ch <= '9') {
            nibble = (uint32_t)(ch - '0');
        } else if (ch >= 'a' && ch <= 'f') {
            nibble = (uint32_t)(ch - 'a' + 10);
        } else if (ch >= 'A' && ch <= 'F') {
            nibble = (uint32_t)(ch - 'A' + 10);
        } else {
            return 0;
        }
        for (bit = 0u; bit < 4u; ++bit) {
            const uint32_t position =
                (uint32_t)((size - 1u - index) * 4u) + bit;
            bytes_set_bit(bytes, position, ((nibble >> bit) & 1u) != 0u);
        }
    }
    return 1;
}

static int decode_decimal_literal(const char *text, size_t size,
                                  uint32_t width, uint8_t *bytes) {
    uint64_t value = 0u;
    size_t index;

    if (size == 0u || width > 64u) {
        return 0;
    }
    for (index = 0u; index < size; ++index) {
        const char ch = text[index];
        if (ch < '0' || ch > '9') {
            return 0;
        }
        if (value > (UINT64_MAX - (uint64_t)(ch - '0')) / 10u) {
            return 0;
        }
        value = value * 10u + (uint64_t)(ch - '0');
    }
    for (index = 0u; index < width; ++index) {
        bytes_set_bit(bytes, (uint32_t)index,
                      ((value >> index) & 1u) != 0u);
    }
    return 1;
}

/* Accepts every bit-vector literal form Bitwuzla may print for a model, plus
   the two Boolean literals. */
static int decode_model_value(const char *text, size_t size,
                              ql_source_type_kind kind, uint32_t width,
                              uint8_t *bytes, size_t *byte_count) {
    replay_scanner scanner;
    const char *token;
    size_t token_size;

    if (kind == QL_SOURCE_TYPE_BOOL) {
        *byte_count = 1u;
        if (token_equals(text, size, "true")) {
            bytes[0] = 1u;
            return 1;
        }
        if (token_equals(text, size, "false")) {
            bytes[0] = 0u;
            return 1;
        }
        /* A one-bit vector is an acceptable spelling of a Boolean model. */
        if (size == 3u && text[0] == '#' && text[1] == 'b') {
            bytes[0] = text[2] == '1' ? 1u : 0u;
            return text[2] == '0' || text[2] == '1';
        }
        return 0;
    }
    if (width == 0u || width > QL_REPLAY_MAX_INPUT_BYTES * 8u) {
        return 0;
    }
    *byte_count = ((size_t)width + 7u) / 8u;
    memset(bytes, 0, *byte_count);
    if (size > 2u && text[0] == '#' && text[1] == 'b') {
        return decode_binary_literal(text + 2, size - 2u, width, bytes);
    }
    if (size > 2u && text[0] == '#' && text[1] == 'x') {
        return decode_hex_literal(text + 2, size - 2u, width, bytes);
    }
    if (size > 2u && text[0] == '(') {
        /* (_ bvDECIMAL WIDTH) */
        scanner.text = text + 1;
        scanner.size = size - 2u;
        scanner.cursor = 0u;
        if (!scanner_next(&scanner, &token, &token_size) ||
            !token_equals(token, token_size, "_")) {
            return 0;
        }
        if (!scanner_next(&scanner, &token, &token_size) || token_size <= 2u ||
            memcmp(token, "bv", 2u) != 0) {
            return 0;
        }
        return decode_decimal_literal(token + 2, token_size - 2u, width,
                                      bytes);
    }
    return 0;
}

/* --- Memory model decoding ------------------------------------------------ */

static int decode_model_word(const char *text, size_t size, uint32_t width,
                             uint64_t *output) {
    uint8_t bytes[QL_REPLAY_MAX_INPUT_BYTES];
    size_t byte_count = 0u;
    size_t index;

    *output = 0u;
    if (width > 64u ||
        !decode_model_value(text, size, QL_SOURCE_TYPE_UNSIGNED_INTEGER,
                            width, bytes, &byte_count)) {
        return 0;
    }
    for (index = 0u; index < byte_count && index < 8u; ++index) {
        *output |= (uint64_t)bytes[index] << (index * 8u);
    }
    return 1;
}

static int image_record(replay_image *image, const ql_allocator *allocator,
                        uint64_t address, uint8_t byte) {
    size_t index;

    /* Outer stores are visited first and win, so an address already recorded
       keeps its byte. */
    for (index = 0u; index < image->count; ++index) {
        if (image->overrides[index].address == address) {
            return 1;
        }
    }
    if (image->count == QL_REPLAY_MAX_MEMORY_OVERRIDES) {
        return 0;
    }
    if (image->count == image->capacity) {
        const size_t capacity = image->capacity == 0u ? 32u
                                                      : image->capacity * 2u;
        void *allocation = allocator->reallocate(
            allocator->user_data, image->overrides,
            capacity * sizeof(*image->overrides));
        if (allocation == NULL) {
            return 0;
        }
        image->overrides = (replay_override *)allocation;
        image->capacity = capacity;
    }
    image->overrides[image->count].address = address;
    image->overrides[image->count].byte = byte;
    ++image->count;
    return 1;
}

/* `(store (store ((as const (Array ...)) <fill>) <index> <byte>) ...)`, which
   is how Bitwuzla prints an array model. Anything else is refused rather than
   guessed at. */
static int decode_memory_term(const char *text, size_t size,
                              const ql_allocator *allocator,
                              replay_image *image) {
    replay_scanner scanner;
    const char *head;
    size_t head_size;
    const char *index_text;
    size_t index_size;
    const char *value_text;
    size_t value_size;
    uint64_t address;
    uint64_t byte;

    for (;;) {
        if (size < 2u || text[0] != '(') {
            return 0;
        }
        scanner.text = text + 1;
        scanner.size = size - 2u;
        scanner.cursor = 0u;
        if (!scanner_next(&scanner, &head, &head_size)) {
            return 0;
        }
        if (token_equals(head, head_size, "store")) {
            if (!scanner_next(&scanner, &head, &head_size) ||
                !scanner_next(&scanner, &index_text, &index_size) ||
                !scanner_next(&scanner, &value_text, &value_size)) {
                return 0;
            }
            if (!decode_model_word(index_text, index_size, 64u, &address) ||
                !decode_model_word(value_text, value_size, 8u, &byte) ||
                !image_record(image, allocator, address, (uint8_t)byte)) {
                return 0;
            }
            text = head;
            size = head_size;
            continue;
        }
        /* `((as const (Array ...)) <fill>)`: the head is the constant-array
           application and the next datum is the byte it repeats. */
        if (head_size > 2u && head[0] == '(' &&
            scanner_next(&scanner, &value_text, &value_size) &&
            decode_model_word(value_text, value_size, 8u, &byte)) {
            image->fill = (uint8_t)byte;
            return 1;
        }
        return 0;
    }
}

/* Builds the layout the interpreter will run over. It is the model's own
   layout: the standing constraints are re-checked here rather than assumed,
   so a model that somehow escaped them leaves the replay undecided. */
static int build_objects(ql_replay_witness *witness, const replay_image *image,
                         int memory_seen) {
    const ql_allocator *allocator = &witness->allocator;
    size_t index;

    if (witness->object_count == 0u) {
        witness->layout_usable = 1u;
        return 1;
    }
    if (!memory_seen) {
        return 1;
    }
    for (index = 0u; index < witness->object_count; ++index) {
        replay_object *object = &witness->objects[index];
        size_t byte;

        if (object->model_size == 0u ||
            object->model_size > QL_REPLAY_MAX_OBJECT_BYTES ||
            object->base < QL_IR_INTERP_FIRST_OBJECT_ADDRESS ||
            object->model_size > UINT64_MAX - object->base) {
            return 1;
        }
        object->size = object->model_size;
        object->initial = allocator->allocate(allocator->user_data,
                                              (size_t)object->size);
        if (object->initial == NULL) {
            return 0;
        }
        memset(object->initial, image->fill, (size_t)object->size);
        for (byte = 0u; byte < image->count; ++byte) {
            const uint64_t address = image->overrides[byte].address;
            if (address < object->base ||
                address - object->base >= object->size) {
                continue;
            }
            object->initial[address - object->base] =
                image->overrides[byte].byte;
        }
    }
    for (index = 1u; index < witness->object_count; ++index) {
        size_t other;
        for (other = 0u; other < index; ++other) {
            const replay_object *a = &witness->objects[other];
            const replay_object *b = &witness->objects[index];
            if (b->base < a->base + a->size && a->base < b->base + b->size) {
                return 1;
            }
        }
    }
    witness->layout_usable = 1u;
    return 1;
}

ql_status QL_CALL ql_replay_decode_model(const ql_allocator *allocator,
                                         const ql_product_query *query,
                                         const ql_artifact *model,
                                         ql_replay_witness **output,
                                         ql_error *error) {
    const ql_allocator *selected = select_allocator(allocator);
    ql_product_query_view_v1 query_view;
    ql_artifact_view model_view;
    ql_replay_witness *witness;
    uint8_t *seen = NULL;
    replay_scanner scanner;
    replay_image image;
    const char *memory_symbol;
    int memory_seen = 0;
    size_t index;
    ql_status status;

    if (output == NULL || query == NULL || model == NULL) {
        ql_error_set(error, QL_STATUS_INVALID_ARGUMENT,
                     "query, model artifact, and witness output are required");
        return QL_STATUS_INVALID_ARGUMENT;
    }
    *output = NULL;
    memset(&query_view, 0, sizeof(query_view));
    query_view.struct_size = sizeof(query_view);
    status = ql_product_query_get_view(query, &query_view, error);
    if (status != QL_STATUS_OK) {
        return status;
    }
    memset(&model_view, 0, sizeof(model_view));
    model_view.struct_size = sizeof(model_view);
    status = ql_artifact_get_view(model, &model_view, error);
    if (status != QL_STATUS_OK) {
        return status;
    }
    if (strcmp(model_view.kind, QL_ARTIFACT_KIND_SOLVER_MODEL) != 0) {
        ql_error_set(error, QL_STATUS_TYPE_MISMATCH,
                     "artifact is not a quodlibet.solver-model");
        return QL_STATUS_TYPE_MISMATCH;
    }
    if (!ql_allocator_is_valid(selected)) {
        ql_error_set(error, QL_STATUS_INVALID_ARGUMENT, "invalid allocator");
        return QL_STATUS_INVALID_ARGUMENT;
    }

    witness = selected->allocate(selected->user_data, sizeof(*witness));
    if (witness == NULL) {
        ql_error_set(error, QL_STATUS_OUT_OF_MEMORY, NULL);
        return QL_STATUS_OUT_OF_MEMORY;
    }
    memset(witness, 0, sizeof(*witness));
    memset(&image, 0, sizeof(image));
    witness->allocator = *selected;
    witness->count = query_view.input_count;
    witness->object_count = ql_product_query_object_count(query);
    memory_symbol = ql_product_query_memory_symbol(query);
    if (witness->object_count != 0u) {
        witness->objects = selected->allocate(
            selected->user_data,
            witness->object_count * sizeof(*witness->objects));
        if (witness->objects == NULL) {
            ql_error_set(error, QL_STATUS_OUT_OF_MEMORY, NULL);
            status = QL_STATUS_OUT_OF_MEMORY;
            goto cleanup;
        }
        memset(witness->objects, 0,
               witness->object_count * sizeof(*witness->objects));
        for (index = 0u; index < witness->object_count; ++index) {
            ql_product_object_v1 object;
            status = ql_product_query_object_at(query, index, &object, error);
            if (status != QL_STATUS_OK) {
                goto cleanup;
            }
            witness->objects[index].left_base_parameter =
                object.left_base_parameter;
            witness->objects[index].right_base_parameter =
                object.right_base_parameter;
        }
    }
    if (witness->count != 0u) {
        witness->values = selected->allocate(
            selected->user_data, witness->count * sizeof(*witness->values));
        seen = selected->allocate(selected->user_data, witness->count);
        if (witness->values == NULL || seen == NULL) {
            ql_error_set(error, QL_STATUS_OUT_OF_MEMORY, NULL);
            status = QL_STATUS_OUT_OF_MEMORY;
            goto cleanup;
        }
        memset(witness->values, 0,
               witness->count * sizeof(*witness->values));
        memset(seen, 0, witness->count);
    }
    for (index = 0u; index < witness->count; ++index) {
        ql_product_input_v1 input;
        status = ql_product_query_input_at(query, index, &input, error);
        if (status != QL_STATUS_OK) {
            goto cleanup;
        }
        witness->values[index].struct_size =
            sizeof(witness->values[index]);
        witness->values[index].index = input.index;
        witness->values[index].left_parameter = input.left_parameter;
        witness->values[index].right_parameter = input.right_parameter;
        witness->values[index].kind = input.kind;
        witness->values[index].bit_width = input.bit_width;
    }

    scanner.text = (const char *)model_view.data;
    scanner.size = model_view.size;
    scanner.cursor = 0u;
    for (;;) {
        const char *token;
        size_t token_size;
        replay_scanner inner;
        const char *symbol;
        size_t symbol_size;
        const char *value;
        size_t value_size;

        if (!scanner_next(&scanner, &token, &token_size)) {
            break;
        }
        if (token_size < 2u || token[0] != '(') {
            continue;
        }
        inner.text = token + 1;
        inner.size = token_size - 2u;
        inner.cursor = 0u;
        if (!scanner_next(&inner, &value, &value_size)) {
            continue;
        }
        if (!token_equals(value, value_size, "define-fun")) {
            /* The model is wrapped in one outer list; descend into it. */
            scanner.text = token + 1;
            scanner.size = token_size - 2u;
            scanner.cursor = 0u;
            continue;
        }
        if (!scanner_next(&inner, &symbol, &symbol_size) ||
            !scanner_next(&inner, &value, &value_size) ||
            !scanner_next(&inner, &value, &value_size) ||
            !scanner_next(&inner, &value, &value_size)) {
            continue;
        }
        for (index = 0u; index < witness->count; ++index) {
            ql_product_input_v1 input;
            status = ql_product_query_input_at(query, index, &input, error);
            if (status != QL_STATUS_OK) {
                goto cleanup;
            }
            if (!token_equals(symbol, symbol_size, input.symbol)) {
                continue;
            }
            if (!decode_model_value(value, value_size, input.kind,
                                    input.bit_width,
                                    witness->values[index].bytes,
                                    &witness->values[index].size)) {
                ql_error_set(error, QL_STATUS_PARSE_ERROR,
                             "solver model assigns '%s' a value this decoder does not accept",
                             input.symbol);
                status = QL_STATUS_PARSE_ERROR;
                goto cleanup;
            }
            seen[index] = 1u;
            break;
        }
        if (memory_symbol != NULL &&
            token_equals(symbol, symbol_size, memory_symbol)) {
            /* A memory the decoder cannot read is not an error about the two
               functions; it leaves the replay inconclusive. */
            memory_seen = decode_memory_term(value, value_size, selected,
                                             &image);
            continue;
        }
        for (index = 0u; index < witness->object_count; ++index) {
            ql_product_object_v1 object;
            uint64_t word = 0u;
            int is_base;
            status = ql_product_query_object_at(query, index, &object, error);
            if (status != QL_STATUS_OK) {
                goto cleanup;
            }
            is_base = token_equals(symbol, symbol_size, object.base_symbol);
            if (!is_base &&
                !token_equals(symbol, symbol_size, object.size_symbol)) {
                continue;
            }
            if (!decode_model_word(value, value_size, object.address_width,
                                   &word)) {
                ql_error_set(error, QL_STATUS_PARSE_ERROR,
                             "solver model assigns '%s' a value this decoder does not accept",
                             is_base ? object.base_symbol
                                     : object.size_symbol);
                status = QL_STATUS_PARSE_ERROR;
                goto cleanup;
            }
            if (is_base) {
                witness->objects[index].base = word;
            } else {
                witness->objects[index].model_size = word;
            }
            break;
        }
    }
    for (index = 0u; index < witness->count; ++index) {
        if (seen[index] == 0u) {
            ql_error_set(error, QL_STATUS_PARSE_ERROR,
                         "solver model does not assign input %zu", index);
            status = QL_STATUS_PARSE_ERROR;
            goto cleanup;
        }
    }
    if (!build_objects(witness, &image, memory_seen)) {
        ql_error_set(error, QL_STATUS_OUT_OF_MEMORY, NULL);
        status = QL_STATUS_OUT_OF_MEMORY;
        goto cleanup;
    }
    selected->deallocate(selected->user_data, seen);
    selected->deallocate(selected->user_data, image.overrides);
    *output = witness;
    ql_error_clear(error);
    return QL_STATUS_OK;

cleanup:
    selected->deallocate(selected->user_data, seen);
    selected->deallocate(selected->user_data, image.overrides);
    ql_replay_witness_destroy(witness);
    return status;
}

void QL_CALL ql_replay_witness_destroy(ql_replay_witness *witness) {
    ql_allocator allocator;
    size_t index;

    if (witness == NULL) {
        return;
    }
    allocator = witness->allocator;
    for (index = 0u; index < witness->object_count; ++index) {
        allocator.deallocate(allocator.user_data,
                             witness->objects[index].initial);
    }
    allocator.deallocate(allocator.user_data, witness->objects);
    allocator.deallocate(allocator.user_data, witness->values);
    allocator.deallocate(allocator.user_data, witness);
}

size_t QL_CALL ql_replay_witness_count(const ql_replay_witness *witness) {
    return witness == NULL ? 0u : witness->count;
}

ql_status QL_CALL ql_replay_witness_value_at(const ql_replay_witness *witness,
                                             size_t index,
                                             ql_replay_value_v1 *output,
                                             ql_error *error) {
    if (witness == NULL || output == NULL) {
        ql_error_set(error, QL_STATUS_INVALID_ARGUMENT,
                     "witness and value output are required");
        return QL_STATUS_INVALID_ARGUMENT;
    }
    if (index >= witness->count) {
        ql_error_set(error, QL_STATUS_NOT_FOUND, "witness has no value %zu",
                     index);
        return QL_STATUS_NOT_FOUND;
    }
    *output = witness->values[index];
    ql_error_clear(error);
    return QL_STATUS_OK;
}

/* --- Concrete precondition evaluation ------------------------------------- */

/* Re-checking the precondition here catches a precondition-encoding defect
   that would otherwise let the solver hand back a witness the problem never
   admitted. */
typedef struct replay_scalar {
    uint64_t value;
    uint32_t width;
    uint32_t is_boolean;
} replay_scalar;

static uint64_t scalar_mask(uint32_t width) {
    return width >= 64u ? UINT64_MAX : ((UINT64_C(1) << width) - 1u);
}

static uint64_t scalar_sign_extend(uint64_t value, uint32_t width) {
    if (width == 0u || width >= 64u) {
        return value;
    }
    if (((value >> (width - 1u)) & 1u) == 0u) {
        return value;
    }
    return value | ~scalar_mask(width);
}

static int evaluate_precondition_node(const ql_precondition *precondition,
                                      uint32_t node_index,
                                      const ql_replay_witness *witness,
                                      replay_scalar *output,
                                      ql_error *error) {
    ql_precondition_node_view_v1 node;
    replay_scalar left;
    replay_scalar right;
    size_t index;
    int negative = 0;
    uint64_t magnitude = 0u;

    memset(&node, 0, sizeof(node));
    node.struct_size = sizeof(node);
    if (ql_precondition_node_at(precondition, node_index, &node, error) !=
        QL_STATUS_OK) {
        return 0;
    }
    memset(output, 0, sizeof(*output));
    switch (node.kind) {
    case QL_PRECONDITION_NODE_BOOL:
        output->is_boolean = 1u;
        output->width = 1u;
        output->value = node.boolean_value != 0u ? 1u : 0u;
        return 1;
    case QL_PRECONDITION_NODE_ARGUMENT:
        if ((size_t)node.argument_index >= witness->count) {
            return 0;
        }
        {
            const ql_replay_value_v1 *value =
                &witness->values[node.argument_index];
            size_t byte;
            if (value->kind == QL_SOURCE_TYPE_BOOL) {
                output->is_boolean = 1u;
                output->width = 1u;
                output->value = value->bytes[0] != 0u ? 1u : 0u;
                return 1;
            }
            if (value->bit_width > QL_REPLAY_MAX_PRECONDITION_BITS) {
                return 0;
            }
            output->width = value->bit_width;
            for (byte = 0u; byte < value->size && byte < 8u; ++byte) {
                output->value |= (uint64_t)value->bytes[byte] << (byte * 8u);
            }
            output->value &= scalar_mask(output->width);
            return 1;
        }
    case QL_PRECONDITION_NODE_INTEGER:
        if (node.bit_width > QL_REPLAY_MAX_PRECONDITION_BITS ||
            node.integer_value_size == 0u) {
            return 0;
        }
        for (index = 0u; index < node.integer_value_size; ++index) {
            const char ch = node.integer_value[index];
            if (index == 0u && ch == '-') {
                negative = 1;
                continue;
            }
            if (ch < '0' || ch > '9') {
                return 0;
            }
            if (magnitude > (UINT64_MAX - (uint64_t)(ch - '0')) / 10u) {
                return 0;
            }
            magnitude = magnitude * 10u + (uint64_t)(ch - '0');
        }
        output->width = node.bit_width;
        output->value = negative != 0 ? (uint64_t)(0u - magnitude) : magnitude;
        output->value &= scalar_mask(output->width);
        return 1;
    default:
        break;
    }

    if (node.child_count == 0u || node.child_count > 2u) {
        return 0;
    }
    if (!evaluate_precondition_node(precondition, node.children[0], witness,
                                    &left, error)) {
        return 0;
    }
    if (node.child_count == 2u &&
        !evaluate_precondition_node(precondition, node.children[1], witness,
                                    &right, error)) {
        return 0;
    }
    output->is_boolean = 1u;
    output->width = 1u;
    switch (node.kind) {
    case QL_PRECONDITION_NODE_NOT:
        output->value = left.value != 0u ? 0u : 1u;
        return 1;
    case QL_PRECONDITION_NODE_AND:
        output->value = (left.value != 0u && right.value != 0u) ? 1u : 0u;
        return 1;
    case QL_PRECONDITION_NODE_OR:
        output->value = (left.value != 0u || right.value != 0u) ? 1u : 0u;
        return 1;
    case QL_PRECONDITION_NODE_IMPLIES:
        output->value = (left.value == 0u || right.value != 0u) ? 1u : 0u;
        return 1;
    case QL_PRECONDITION_NODE_EQUAL:
        output->value = left.value == right.value ? 1u : 0u;
        return 1;
    case QL_PRECONDITION_NODE_NOT_EQUAL:
        output->value = left.value != right.value ? 1u : 0u;
        return 1;
    case QL_PRECONDITION_NODE_UNSIGNED_LESS:
        output->value = left.value < right.value ? 1u : 0u;
        return 1;
    case QL_PRECONDITION_NODE_UNSIGNED_LESS_EQUAL:
        output->value = left.value <= right.value ? 1u : 0u;
        return 1;
    case QL_PRECONDITION_NODE_UNSIGNED_GREATER:
        output->value = left.value > right.value ? 1u : 0u;
        return 1;
    case QL_PRECONDITION_NODE_UNSIGNED_GREATER_EQUAL:
        output->value = left.value >= right.value ? 1u : 0u;
        return 1;
    case QL_PRECONDITION_NODE_SIGNED_LESS:
    case QL_PRECONDITION_NODE_SIGNED_LESS_EQUAL:
    case QL_PRECONDITION_NODE_SIGNED_GREATER:
    case QL_PRECONDITION_NODE_SIGNED_GREATER_EQUAL: {
        const int64_t signed_left =
            (int64_t)scalar_sign_extend(left.value, left.width);
        const int64_t signed_right =
            (int64_t)scalar_sign_extend(right.value, right.width);
        if (node.kind == QL_PRECONDITION_NODE_SIGNED_LESS) {
            output->value = signed_left < signed_right ? 1u : 0u;
        } else if (node.kind == QL_PRECONDITION_NODE_SIGNED_LESS_EQUAL) {
            output->value = signed_left <= signed_right ? 1u : 0u;
        } else if (node.kind == QL_PRECONDITION_NODE_SIGNED_GREATER) {
            output->value = signed_left > signed_right ? 1u : 0u;
        } else {
            output->value = signed_left >= signed_right ? 1u : 0u;
        }
        return 1;
    }
    case QL_PRECONDITION_NODE_SIGNED_ADD:
    case QL_PRECONDITION_NODE_UNSIGNED_ADD:
        output->is_boolean = 0u;
        output->width = node.bit_width;
        output->value = (left.value + right.value) & scalar_mask(node.bit_width);
        return 1;
    case QL_PRECONDITION_NODE_SIGNED_SUBTRACT:
    case QL_PRECONDITION_NODE_UNSIGNED_SUBTRACT:
        output->is_boolean = 0u;
        output->width = node.bit_width;
        output->value = (left.value - right.value) & scalar_mask(node.bit_width);
        return 1;
    case QL_PRECONDITION_NODE_SIGNED_MULTIPLY:
    case QL_PRECONDITION_NODE_UNSIGNED_MULTIPLY:
        output->is_boolean = 0u;
        output->width = node.bit_width;
        output->value = (left.value * right.value) & scalar_mask(node.bit_width);
        return 1;
    default:
        return 0;
    }
}

static int check_precondition(const ql_allocator *allocator,
                              const ql_problem *problem,
                              const ql_replay_witness *witness,
                              uint32_t *holds, ql_error *error) {
    ql_signature_argument_v1 storage[QL_REPLAY_MAX_PARAMETERS];
    ql_signature_view_v1 signature_view;
    ql_precondition_view_v1 precondition_view;
    ql_source_signature *signature = NULL;
    ql_precondition *precondition = NULL;
    ql_problem_view_v2 problem_view;
    replay_scalar value;
    int evaluated = 0;

    *holds = 0u;
    memset(&problem_view, 0, sizeof(problem_view));
    problem_view.struct_size = sizeof(problem_view);
    if (ql_problem_get_view_v2(problem, &problem_view, error) !=
        QL_STATUS_OK) {
        return 0;
    }
    if (ql_source_signature_open(allocator,
                                 ql_problem_left_signature_artifact(problem),
                                 &signature, error) != QL_STATUS_OK) {
        return 0;
    }
    memset(&signature_view, 0, sizeof(signature_view));
    if (ql_source_signature_precondition_view(
            signature, &signature_view, storage, QL_REPLAY_MAX_PARAMETERS,
            error) == QL_STATUS_OK &&
        ql_precondition_parse(allocator,
                              problem_view.contract.precondition_json,
                              problem_view.contract.precondition_json_size,
                              &signature_view, &precondition,
                              error) == QL_STATUS_OK) {
        memset(&precondition_view, 0, sizeof(precondition_view));
        precondition_view.struct_size = sizeof(precondition_view);
        if (ql_precondition_get_view(precondition, &precondition_view,
                                     error) == QL_STATUS_OK &&
            evaluate_precondition_node(precondition,
                                       precondition_view.root_node, witness,
                                       &value, error)) {
            *holds = value.value != 0u ? 1u : 0u;
            evaluated = 1;
        }
    }
    ql_precondition_destroy(precondition);
    ql_source_signature_release(signature);
    return evaluated;
}

/* --- Concrete execution --------------------------------------------------- */

static ql_status collect_parameters(const ql_ir *ir,
                                    ql_ir_value_id *parameters,
                                    size_t capacity, size_t *count,
                                    ql_error *error) {
    ql_ir_view_v1 view;
    size_t index;
    ql_status status;

    *count = 0u;
    memset(&view, 0, sizeof(view));
    view.struct_size = sizeof(view);
    status = ql_ir_get_view(ir, &view, error);
    if (status != QL_STATUS_OK) {
        return status;
    }
    for (index = 0u; index < view.value_count; ++index) {
        ql_ir_value_view_v1 value;
        memset(&value, 0, sizeof(value));
        value.struct_size = sizeof(value);
        status = ql_ir_value_at(ir, index, &value, error);
        if (status != QL_STATUS_OK) {
            return status;
        }
        if (value.definition_kind != QL_IR_VALUE_PARAMETER) {
            continue;
        }
        if (*count == capacity) {
            ql_error_set(error, QL_STATUS_TYPE_MISMATCH,
                         "IR declares more parameters than the replay accepts");
            return QL_STATUS_TYPE_MISMATCH;
        }
        parameters[(*count)++] = value.id;
    }
    return QL_STATUS_OK;
}

static ql_status trap_code_of(const ql_ir *ir, ql_ir_block_id block,
                              uint64_t *code, ql_error *error) {
    ql_ir_block_view_v1 view;
    ql_status status;

    *code = 0u;
    memset(&view, 0, sizeof(view));
    view.struct_size = sizeof(view);
    status = ql_ir_block_at(ir, block, &view, error);
    if (status != QL_STATUS_OK) {
        return status;
    }
    *code = view.terminator.code;
    return QL_STATUS_OK;
}

static void encode_word(uint8_t *bytes, uint64_t value) {
    size_t index;

    for (index = 0u; index < 8u; ++index) {
        bytes[index] = (uint8_t)((value >> (index * 8u)) & 0xffu);
    }
}

/* `final_images` receives one buffer per object when the caller observes
   memory, and is null otherwise. */
static ql_status run_side(const ql_allocator *allocator, const ql_ir *ir,
                          const ql_replay_witness *witness, int is_left,
                          uint8_t **final_images,
                          ql_replay_outcome_v1 *outcome, ql_error *error) {
    ql_ir_value_id parameters[QL_REPLAY_MAX_PARAMETERS];
    ql_ir_interp_input_v1 inputs[QL_REPLAY_MAX_PARAMETERS];
    ql_ir_interp_object_v1 objects[QL_REPLAY_MAX_PARAMETERS];
    uint8_t object_words[QL_REPLAY_MAX_PARAMETERS][8];
    ql_ir_interp_options_v1 options;
    ql_ir_interp_result_v1 result;
    size_t parameter_count = 0u;
    size_t expected;
    size_t bound = 0u;
    size_t index;
    ql_status status;

    memset(outcome, 0, sizeof(*outcome));
    outcome->struct_size = sizeof(*outcome);
    status = collect_parameters(ir, parameters, QL_REPLAY_MAX_PARAMETERS,
                                &parameter_count, error);
    if (status != QL_STATUS_OK) {
        return status;
    }
    expected = witness->count +
               (witness->object_count == 0u ? 0u
                                            : 1u + 2u * witness->object_count);
    if (parameter_count != expected || expected > QL_REPLAY_MAX_PARAMETERS) {
        ql_error_set(error, QL_STATUS_TYPE_MISMATCH,
                     "IR declares %zu parameters but the witness and its object table imply %zu",
                     parameter_count, expected);
        return QL_STATUS_TYPE_MISMATCH;
    }
    for (index = 0u; index < witness->count; ++index) {
        const ql_replay_value_v1 *value = &witness->values[index];
        const uint32_t ordinal =
            is_left != 0 ? value->left_parameter : value->right_parameter;
        if ((size_t)ordinal >= parameter_count) {
            ql_error_set(error, QL_STATUS_TYPE_MISMATCH,
                         "argument correspondence names parameter %u outside the IR",
                         ordinal);
            return QL_STATUS_TYPE_MISMATCH;
        }
        ql_ir_interp_input_init(&inputs[bound]);
        inputs[bound].value = parameters[ordinal];
        inputs[bound].data = value->bytes;
        inputs[bound].size = value->size;
        ++bound;
    }
    ql_ir_interp_options_init(&options);
    if (witness->object_count != 0u) {
        /* The memory parameter sits immediately after the C arguments and
           carries no bytes; the object table supplies the initial image. */
        ql_ir_interp_input_init(&inputs[bound]);
        inputs[bound].value = parameters[witness->count];
        ++bound;
        for (index = 0u; index < witness->object_count; ++index) {
            const replay_object *object = &witness->objects[index];
            const uint32_t base_ordinal = is_left != 0
                                              ? object->left_base_parameter
                                              : object->right_base_parameter;
            if ((size_t)base_ordinal + 1u >= parameter_count) {
                ql_error_set(error, QL_STATUS_TYPE_MISMATCH,
                             "the object table names parameter %u outside the IR",
                             base_ordinal);
                return QL_STATUS_TYPE_MISMATCH;
            }
            encode_word(object_words[index * 2u], object->base);
            encode_word(object_words[index * 2u + 1u], object->size);
            ql_ir_interp_input_init(&inputs[bound]);
            inputs[bound].value = parameters[base_ordinal];
            inputs[bound].data = object_words[index * 2u];
            inputs[bound].size = 8u;
            ++bound;
            ql_ir_interp_input_init(&inputs[bound]);
            inputs[bound].value = parameters[base_ordinal + 1u];
            inputs[bound].data = object_words[index * 2u + 1u];
            inputs[bound].size = 8u;
            ++bound;
            ql_ir_interp_object_init(&objects[index]);
            objects[index].base = object->base;
            objects[index].size = object->size;
            objects[index].initial = object->initial;
            objects[index].final_image =
                final_images != NULL ? final_images[index] : NULL;
        }
        options.objects = objects;
        options.object_count = witness->object_count;
    }
    memset(&result, 0, sizeof(result));
    result.struct_size = sizeof(result);
    status = ql_ir_interp_run(allocator, ir, inputs, bound, &options, &result,
                              error);
    if (status != QL_STATUS_OK) {
        return status;
    }
    outcome->interp_outcome = result.outcome;
    outcome->ub_reason = result.ub_reason;
    switch (result.outcome) {
    case QL_IR_INTERP_OUTCOME_RETURN:
        outcome->conclusive = 1u;
        outcome->defined = 1u;
        outcome->returns = 1u;
        outcome->terminates = 1u;
        outcome->return_value_size = result.value_size;
        if (result.value_size > sizeof(outcome->return_value)) {
            ql_error_set(error, QL_STATUS_INTERNAL_ERROR,
                         "interpreter returned an oversized value");
            return QL_STATUS_INTERNAL_ERROR;
        }
        memcpy(outcome->return_value, result.value, result.value_size);
        break;
    case QL_IR_INTERP_OUTCOME_TRAP:
        outcome->conclusive = 1u;
        outcome->defined = 1u;
        outcome->traps = 1u;
        outcome->terminates = 1u;
        status = trap_code_of(ir, result.block, &outcome->trap_code, error);
        if (status != QL_STATUS_OK) {
            return status;
        }
        break;
    case QL_IR_INTERP_OUTCOME_DIVERGE:
        outcome->conclusive = 1u;
        outcome->defined = 1u;
        break;
    case QL_IR_INTERP_OUTCOME_UNDEFINED_BEHAVIOR:
        /* Undefined is a conclusion about the execution, whichever guard
           reported it. The reason is preserved so a caller can log a
           lowering defect separately from ordinary source-level UB. */
        outcome->conclusive = 1u;
        outcome->defined = 0u;
        outcome->terminates = 1u;
        break;
    default:
        /* TERMINATE, an assumption rejection, an unsupported construct, and
           the step limit are all statements about this tool, not about the
           function. */
        outcome->conclusive = 0u;
        break;
    }
    return QL_STATUS_OK;
}

static uint32_t observations_agree(const ql_replay_outcome_v1 *left,
                                   const ql_replay_outcome_v1 *right,
                                   uint64_t observations,
                                   uint32_t memory_agrees) {
    if ((observations & QL_OBSERVE_MEMORY) != 0u &&
        left->terminates != 0u && right->terminates != 0u &&
        memory_agrees == 0u) {
        return 0u;
    }
    if ((observations & QL_OBSERVE_RETURN_VALUE) != 0u) {
        if (left->returns != right->returns) {
            return 0u;
        }
        if (left->returns != 0u &&
            (left->return_value_size != right->return_value_size ||
             memcmp(left->return_value, right->return_value,
                    left->return_value_size) != 0)) {
            return 0u;
        }
    }
    if ((observations & QL_OBSERVE_TERMINATION) != 0u &&
        left->terminates != right->terminates) {
        return 0u;
    }
    if ((observations & QL_OBSERVE_TRAPS) != 0u) {
        if (left->traps != right->traps) {
            return 0u;
        }
        if (left->traps != 0u && left->trap_code != right->trap_code) {
            return 0u;
        }
    }
    return 1u;
}

/* Mirrors src/product.c encode_violation() on concrete values. The two must
   agree; a disagreement is exactly what this replay exists to catch. */
static uint32_t relation_violated(const ql_product_query_view_v1 *query,
                                  const ql_replay_outcome_v1 *left,
                                  const ql_replay_outcome_v1 *right,
                                  uint32_t memory_agrees) {
    const uint32_t agree = observations_agree(
        left, right, query->covered_observations, memory_agrees);

    switch (query->ub_policy) {
    case QL_UB_MUST_MATCH:
        if (left->defined != right->defined) {
            return 1u;
        }
        return (left->defined != 0u && agree == 0u) ? 1u : 0u;
    case QL_UB_LANGUAGE_REFINEMENT:
        switch (query->relation) {
        case QL_RELATION_LEFT_REFINES_RIGHT:
            return (right->defined != 0u &&
                    (left->defined == 0u || agree == 0u))
                       ? 1u
                       : 0u;
        case QL_RELATION_RIGHT_REFINES_LEFT:
            return (left->defined != 0u &&
                    (right->defined == 0u || agree == 0u))
                       ? 1u
                       : 0u;
        default:
            if (left->defined != right->defined) {
                return 1u;
            }
            return (left->defined != 0u && agree == 0u) ? 1u : 0u;
        }
    default:
        return (left->defined != 0u && right->defined != 0u && agree == 0u)
                   ? 1u
                   : 0u;
    }
}

static void release_images(const ql_allocator *allocator,
                           const ql_replay_witness *witness,
                           uint8_t **images) {
    size_t index;

    if (images == NULL) {
        return;
    }
    for (index = 0u; index < witness->object_count; ++index) {
        allocator->deallocate(allocator->user_data, images[index]);
    }
    allocator->deallocate(allocator->user_data, images);
}

static ql_status allocate_images(const ql_allocator *allocator,
                                 const ql_replay_witness *witness,
                                 uint8_t ***output, ql_error *error) {
    uint8_t **images;
    size_t index;

    images = allocator->allocate(allocator->user_data,
                                 witness->object_count * sizeof(*images));
    if (images == NULL) {
        ql_error_set(error, QL_STATUS_OUT_OF_MEMORY, NULL);
        return QL_STATUS_OUT_OF_MEMORY;
    }
    memset(images, 0, witness->object_count * sizeof(*images));
    *output = images;
    for (index = 0u; index < witness->object_count; ++index) {
        images[index] = allocator->allocate(
            allocator->user_data, (size_t)witness->objects[index].size);
        if (images[index] == NULL) {
            ql_error_set(error, QL_STATUS_OUT_OF_MEMORY, NULL);
            return QL_STATUS_OUT_OF_MEMORY;
        }
    }
    return QL_STATUS_OK;
}

ql_status QL_CALL ql_replay_execute(const ql_allocator *allocator,
                                    const ql_problem *problem,
                                    const ql_product_query *query,
                                    const ql_ir *left_ir,
                                    const ql_ir *right_ir,
                                    const ql_replay_witness *witness,
                                    ql_replay_result_v1 *result,
                                    ql_error *error) {
    const ql_allocator *selected = select_allocator(allocator);
    ql_product_query_view_v1 query_view;
    uint8_t **left_images = NULL;
    uint8_t **right_images = NULL;
    uint32_t memory_agrees = 1u;
    int observe_memory;
    size_t index;
    ql_status status;

    if (problem == NULL || query == NULL || left_ir == NULL ||
        right_ir == NULL || witness == NULL || result == NULL) {
        ql_error_set(error, QL_STATUS_INVALID_ARGUMENT,
                     "problem, query, both IR functions, witness, and result are required");
        return QL_STATUS_INVALID_ARGUMENT;
    }
    memset(result, 0, sizeof(*result));
    result->struct_size = sizeof(*result);
    result->schema_version = QL_REPLAY_SCHEMA_VERSION;
    memset(&query_view, 0, sizeof(query_view));
    query_view.struct_size = sizeof(query_view);
    status = ql_product_query_get_view(query, &query_view, error);
    if (status != QL_STATUS_OK) {
        return status;
    }
    if (witness->object_count != 0u && witness->layout_usable == 0u) {
        /* The model's memory did not turn into a layout this replay can run.
           That is a statement about this decoder, not about the functions. */
        result->conclusive = 0u;
        ql_error_clear(error);
        return QL_STATUS_OK;
    }
    observe_memory = witness->object_count != 0u &&
                     (query_view.covered_observations & QL_OBSERVE_MEMORY) !=
                         0u;
    if (observe_memory) {
        status = allocate_images(selected, witness, &left_images, error);
        if (status == QL_STATUS_OK) {
            status = allocate_images(selected, witness, &right_images, error);
        }
        if (status != QL_STATUS_OK) {
            goto cleanup;
        }
    }
    status = run_side(selected, left_ir, witness, 1, left_images,
                      &result->left, error);
    if (status != QL_STATUS_OK) {
        goto cleanup;
    }
    status = run_side(selected, right_ir, witness, 0, right_images,
                      &result->right, error);
    if (status != QL_STATUS_OK) {
        goto cleanup;
    }
    for (index = 0u; observe_memory && index < witness->object_count;
         ++index) {
        if (memcmp(left_images[index], right_images[index],
                   (size_t)witness->objects[index].size) != 0) {
            memory_agrees = 0u;
            break;
        }
    }
    result->precondition_evaluated =
        check_precondition(selected, problem, witness,
                           &result->precondition_holds, error) != 0
            ? 1u
            : 0u;
    if (result->precondition_evaluated == 0u ||
        result->left.conclusive == 0u || result->right.conclusive == 0u ||
        result->precondition_holds == 0u) {
        result->conclusive = 0u;
        status = QL_STATUS_OK;
        ql_error_clear(error);
        goto cleanup;
    }
    result->conclusive = 1u;
    result->violated = relation_violated(&query_view, &result->left,
                                         &result->right, memory_agrees);
    status = QL_STATUS_OK;
    ql_error_clear(error);

cleanup:
    release_images(selected, witness, left_images);
    release_images(selected, witness, right_images);
    return status;
}

/* --- Counterexample artifact ---------------------------------------------- */

static void *replay_json_allocate(void *context, size_t size) {
    ql_allocator *allocator = (ql_allocator *)context;
    return allocator->allocate(allocator->user_data, size);
}

static void *replay_json_reallocate(void *context, void *pointer,
                                    size_t old_size, size_t size) {
    ql_allocator *allocator = (ql_allocator *)context;
    (void)old_size;
    return allocator->reallocate(allocator->user_data, pointer, size);
}

static void replay_json_deallocate(void *context, void *pointer) {
    ql_allocator *allocator = (ql_allocator *)context;
    allocator->deallocate(allocator->user_data, pointer);
}

static void bytes_to_hex(const uint8_t *bytes, size_t size, char *output) {
    static const char digits[] = "0123456789abcdef";
    size_t index;

    for (index = 0u; index < size; ++index) {
        const uint8_t byte = bytes[size - 1u - index];
        output[index * 2u] = digits[(byte >> 4) & 0x0fu];
        output[index * 2u + 1u] = digits[byte & 0x0fu];
    }
    output[size * 2u] = '\0';
}

static int add_outcome_json(yyjson_mut_doc *document, yyjson_mut_val *root,
                            const char *key,
                            const ql_replay_outcome_v1 *outcome) {
    char hex[QL_IR_INTERP_VALUE_CAPACITY * 2u + 1u];
    yyjson_mut_val *object = yyjson_mut_obj(document);

    if (object == NULL ||
        !yyjson_mut_obj_add_str(document, object, "interpreter_outcome",
                                ql_ir_interp_outcome_string(
                                    outcome->interp_outcome)) ||
        !yyjson_mut_obj_add_str(document, object, "ub_reason",
                                ql_ir_interp_ub_reason_string(
                                    outcome->ub_reason)) ||
        !yyjson_mut_obj_add_bool(document, object, "defined",
                                 outcome->defined != 0u) ||
        !yyjson_mut_obj_add_bool(document, object, "returns",
                                 outcome->returns != 0u) ||
        !yyjson_mut_obj_add_bool(document, object, "traps",
                                 outcome->traps != 0u) ||
        !yyjson_mut_obj_add_bool(document, object, "terminates",
                                 outcome->terminates != 0u) ||
        !yyjson_mut_obj_add_uint(document, object, "trap_code",
                                 outcome->trap_code)) {
        return 0;
    }
    if (outcome->returns != 0u) {
        bytes_to_hex(outcome->return_value, outcome->return_value_size, hex);
        if (!yyjson_mut_obj_add_strcpy(document, object, "return_value",
                                       hex)) {
            return 0;
        }
    }
    return yyjson_mut_obj_add_val(document, root, key, object);
}

ql_status QL_CALL ql_replay_counterexample_artifact_create(
    const ql_allocator *allocator, const ql_product_query *query,
    const ql_replay_witness *witness, const ql_replay_result_v1 *result,
    ql_artifact **output, ql_error *error) {
    const ql_allocator *selected = select_allocator(allocator);
    ql_allocator allocator_copy;
    yyjson_alc json_allocator;
    yyjson_mut_doc *document = NULL;
    yyjson_mut_val *root;
    yyjson_mut_val *inputs;
    yyjson_write_err write_error;
    ql_product_query_view_v1 query_view;
    char digest_hex[QL_DIGEST_HEX_SIZE];
    char *json = NULL;
    size_t json_size = 0u;
    size_t index;
    ql_status status;

    if (output == NULL || query == NULL || witness == NULL ||
        result == NULL) {
        ql_error_set(error, QL_STATUS_INVALID_ARGUMENT,
                     "query, witness, result, and artifact output are required");
        return QL_STATUS_INVALID_ARGUMENT;
    }
    *output = NULL;
    if (result->violated == 0u || result->conclusive == 0u) {
        ql_error_set(error, QL_STATUS_INVALID_ARGUMENT,
                     "a counterexample artifact requires a replay that confirmed the violation");
        return QL_STATUS_INVALID_ARGUMENT;
    }
    memset(&query_view, 0, sizeof(query_view));
    query_view.struct_size = sizeof(query_view);
    status = ql_product_query_get_view(query, &query_view, error);
    if (status != QL_STATUS_OK) {
        return status;
    }
    if (!ql_allocator_is_valid(selected)) {
        ql_error_set(error, QL_STATUS_INVALID_ARGUMENT, "invalid allocator");
        return QL_STATUS_INVALID_ARGUMENT;
    }
    allocator_copy = *selected;
    json_allocator.malloc = replay_json_allocate;
    json_allocator.realloc = replay_json_reallocate;
    json_allocator.free = replay_json_deallocate;
    json_allocator.ctx = &allocator_copy;

    document = yyjson_mut_doc_new(&json_allocator);
    root = document != NULL ? yyjson_mut_obj(document) : NULL;
    inputs = document != NULL ? yyjson_mut_arr(document) : NULL;
    ql_digest_hex(&query_view.problem_digest, digest_hex);
    if (document == NULL || root == NULL || inputs == NULL ||
        !yyjson_mut_obj_add_str(document, root, "kind",
                                QL_ARTIFACT_KIND_COUNTEREXAMPLE) ||
        !yyjson_mut_obj_add_uint(document, root, "schema_version",
                                 QL_REPLAY_SCHEMA_VERSION) ||
        !yyjson_mut_obj_add_strcpy(document, root, "problem_digest",
                                   digest_hex) ||
        !yyjson_mut_obj_add_uint(document, root, "relation",
                                 (uint64_t)query_view.relation) ||
        !yyjson_mut_obj_add_uint(document, root, "ub_policy",
                                 (uint64_t)query_view.ub_policy) ||
        !yyjson_mut_obj_add_uint(document, root, "observations",
                                 query_view.covered_observations) ||
        !yyjson_mut_obj_add_bool(document, root, "replayed", 1) ||
        !yyjson_mut_obj_add_bool(document, root, "precondition_holds",
                                 result->precondition_holds != 0u)) {
        status = QL_STATUS_OUT_OF_MEMORY;
        ql_error_set(error, status, NULL);
        goto cleanup;
    }
    for (index = 0u; index < witness->count; ++index) {
        const ql_replay_value_v1 *value = &witness->values[index];
        char hex[QL_REPLAY_MAX_INPUT_BYTES * 2u + 1u];
        yyjson_mut_val *entry = yyjson_mut_obj(document);
        bytes_to_hex(value->bytes, value->size, hex);
        if (entry == NULL ||
            !yyjson_mut_obj_add_uint(document, entry, "index", value->index) ||
            !yyjson_mut_obj_add_uint(document, entry, "left_parameter",
                                     value->left_parameter) ||
            !yyjson_mut_obj_add_uint(document, entry, "right_parameter",
                                     value->right_parameter) ||
            !yyjson_mut_obj_add_uint(document, entry, "kind",
                                     (uint64_t)value->kind) ||
            !yyjson_mut_obj_add_uint(document, entry, "bit_width",
                                     value->bit_width) ||
            !yyjson_mut_obj_add_strcpy(document, entry, "value", hex) ||
            !yyjson_mut_arr_append(inputs, entry)) {
            status = QL_STATUS_OUT_OF_MEMORY;
            ql_error_set(error, status, NULL);
            goto cleanup;
        }
    }
    if (!yyjson_mut_obj_add_val(document, root, "inputs", inputs) ||
        !add_outcome_json(document, root, "left", &result->left) ||
        !add_outcome_json(document, root, "right", &result->right)) {
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
        ql_error_set(error, status,
                     "cannot serialize the counterexample JSON: %s",
                     write_error.msg != NULL ? write_error.msg
                                             : "JSON write error");
        goto cleanup;
    }
    status = ql_artifact_create(selected, QL_ARTIFACT_KIND_COUNTEREXAMPLE,
                                QL_REPLAY_SCHEMA_VERSION, json, json_size,
                                output, error);

cleanup:
    if (json != NULL) {
        json_allocator.free(json_allocator.ctx, json);
    }
    if (document != NULL) {
        yyjson_mut_doc_free(document);
    }
    return status;
}
