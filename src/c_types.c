#include "c_types.h"

#include <string.h>

ql_c_scalar_type ql_c_scalar_make_void(void) {
    ql_c_scalar_type type;
    memset(&type, 0, sizeof(type));
    type.kind = QL_C_SCALAR_VOID;
    return type;
}

ql_c_scalar_type ql_c_scalar_make_bool(void) {
    ql_c_scalar_type type;
    memset(&type, 0, sizeof(type));
    type.kind = QL_C_SCALAR_BOOL;
    type.width = 1u;
    return type;
}

ql_c_scalar_type ql_c_scalar_make_integer(uint32_t width, uint32_t rank,
                                          uint32_t is_signed) {
    ql_c_scalar_type type;
    memset(&type, 0, sizeof(type));
    type.kind = QL_C_SCALAR_INTEGER;
    type.width = width;
    type.rank = rank;
    type.is_signed = is_signed;
    return type;
}

ql_c_scalar_type ql_c_scalar_make_float(uint32_t width, uint32_t rank) {
    ql_c_scalar_type type;
    memset(&type, 0, sizeof(type));
    type.kind = QL_C_SCALAR_FLOAT;
    type.width = width;
    type.rank = rank;
    return type;
}

typedef struct scalar_spelling {
    const char *normalized;
    ql_c_scalar_kind kind;
    uint32_t width;
    uint32_t rank;
    uint32_t is_signed;
} scalar_spelling;

/* Every spelling the profile defines, with whitespace already removed. A name
   absent from this table is not silently treated as int: the caller reports
   it. Widths are the target ABI's, so `long` is 64 bits. */
static const scalar_spelling k_spellings[] = {
    {"void", QL_C_SCALAR_VOID, 0u, 0u, 0u},
    {"_Bool", QL_C_SCALAR_BOOL, 1u, 0u, 0u},
    {"bool", QL_C_SCALAR_BOOL, 1u, 0u, 0u},
    {"char", QL_C_SCALAR_INTEGER, 8u, 1u, 1u},
    {"signedchar", QL_C_SCALAR_INTEGER, 8u, 1u, 1u},
    {"unsignedchar", QL_C_SCALAR_INTEGER, 8u, 1u, 0u},
    {"short", QL_C_SCALAR_INTEGER, 16u, 2u, 1u},
    {"shortint", QL_C_SCALAR_INTEGER, 16u, 2u, 1u},
    {"signedshort", QL_C_SCALAR_INTEGER, 16u, 2u, 1u},
    {"signedshortint", QL_C_SCALAR_INTEGER, 16u, 2u, 1u},
    {"unsignedshort", QL_C_SCALAR_INTEGER, 16u, 2u, 0u},
    {"unsignedshortint", QL_C_SCALAR_INTEGER, 16u, 2u, 0u},
    {"int", QL_C_SCALAR_INTEGER, 32u, 3u, 1u},
    {"signed", QL_C_SCALAR_INTEGER, 32u, 3u, 1u},
    {"signedint", QL_C_SCALAR_INTEGER, 32u, 3u, 1u},
    {"unsigned", QL_C_SCALAR_INTEGER, 32u, 3u, 0u},
    {"unsignedint", QL_C_SCALAR_INTEGER, 32u, 3u, 0u},
    {"long", QL_C_SCALAR_INTEGER, 64u, 4u, 1u},
    {"longint", QL_C_SCALAR_INTEGER, 64u, 4u, 1u},
    {"signedlong", QL_C_SCALAR_INTEGER, 64u, 4u, 1u},
    {"signedlongint", QL_C_SCALAR_INTEGER, 64u, 4u, 1u},
    {"unsignedlong", QL_C_SCALAR_INTEGER, 64u, 4u, 0u},
    {"unsignedlongint", QL_C_SCALAR_INTEGER, 64u, 4u, 0u},
    {"longlong", QL_C_SCALAR_INTEGER, 64u, 5u, 1u},
    {"longlongint", QL_C_SCALAR_INTEGER, 64u, 5u, 1u},
    {"signedlonglong", QL_C_SCALAR_INTEGER, 64u, 5u, 1u},
    {"signedlonglongint", QL_C_SCALAR_INTEGER, 64u, 5u, 1u},
    {"unsignedlonglong", QL_C_SCALAR_INTEGER, 64u, 5u, 0u},
    {"unsignedlonglongint", QL_C_SCALAR_INTEGER, 64u, 5u, 0u},
    {"float", QL_C_SCALAR_FLOAT, 32u, 6u, 0u},
    {"double", QL_C_SCALAR_FLOAT, 64u, 7u, 0u},
    {"longdouble", QL_C_SCALAR_FLOAT, 80u, 8u, 0u}
};

int ql_c_scalar_from_spelling(const char *spelling,
                              ql_c_scalar_type *output) {
    char normalized[QL_C_SCALAR_SPELLING_CAPACITY];
    size_t source_index;
    size_t target_index = 0u;
    size_t index;

    if (spelling == NULL || output == NULL) {
        return 0;
    }
    for (source_index = 0u; spelling[source_index] != '\0'; ++source_index) {
        char character = spelling[source_index];
        if (character == ' ' || character == '\t' || character == '\r' ||
            character == '\n' || character == '\f' || character == '\v') {
            continue;
        }
        if (target_index + 1u >= sizeof(normalized)) {
            return 0;
        }
        normalized[target_index++] = character;
    }
    normalized[target_index] = '\0';

    for (index = 0u; index < sizeof(k_spellings) / sizeof(k_spellings[0]);
         ++index) {
        const scalar_spelling *entry = &k_spellings[index];
        if (strcmp(normalized, entry->normalized) != 0) {
            continue;
        }
        memset(output, 0, sizeof(*output));
        output->kind = entry->kind;
        output->width = entry->width;
        output->rank = entry->rank;
        output->is_signed = entry->is_signed;
        return 1;
    }
    return 0;
}

/* The AnghaBench preamble, transcribed. Every source in the corpus opens with
   the same fixed block, and record extraction drops it, which is why the names
   below reach the lowering undeclared:

       typedef unsigned long size_t;  // Customize by platform.
       typedef long intptr_t; typedef unsigned long uintptr_t;
       typedef long scalar_t__;  // Either arithmetic or pointer type.

   These are transcriptions, not guesses. In a 2,000 file sample every file
   that mentions `size_t` also declares it, all 1,981 of them with that one
   spelling and no other anywhere; `intptr_t` and `uintptr_t` come from the
   same single line in the same 1,981; `scalar_t__` was measured earlier at
   3,998 of 4,000. Widths are the profile's LP64 target, so `long` is 64 bits,
   and `scalar_t__` is wide enough to hold the pointers its comment admits.

   A unit's own declaration still wins: the caller consults this table only
   after failing to find the name among the typedefs the unit declares. */
static const struct {
    const char *spelling;
    uint32_t width;
    uint32_t rank;
    uint32_t is_signed;
} k_corpus_typedefs[] = {
    {"scalar_t__", 64u, 4u, 1u},
    {"size_t", 64u, 4u, 0u},
    {"intptr_t", 64u, 4u, 1u},
    {"uintptr_t", 64u, 4u, 0u}
};

int ql_c_scalar_from_corpus_typedef(const char *spelling,
                                    ql_c_scalar_type *output) {
    size_t index;

    if (spelling == NULL || output == NULL) {
        return 0;
    }
    for (index = 0u;
         index < sizeof(k_corpus_typedefs) / sizeof(k_corpus_typedefs[0]);
         ++index) {
        if (strcmp(spelling, k_corpus_typedefs[index].spelling) != 0) {
            continue;
        }
        *output = ql_c_scalar_make_integer(k_corpus_typedefs[index].width,
                                           k_corpus_typedefs[index].rank,
                                           k_corpus_typedefs[index].is_signed);
        return 1;
    }
    return 0;
}

int ql_c_scalar_same(ql_c_scalar_type left, ql_c_scalar_type right) {
    return left.kind == right.kind && left.width == right.width &&
           left.rank == right.rank && left.is_signed == right.is_signed;
}

ql_c_scalar_type ql_c_scalar_promote(ql_c_scalar_type type) {
    if (type.kind == QL_C_SCALAR_FLOAT) {
        return type;
    }
    if (type.kind == QL_C_SCALAR_BOOL || type.rank < 3u) {
        return ql_c_scalar_make_integer(32u, 3u, 1u);
    }
    return type;
}

ql_c_scalar_type ql_c_scalar_usual(ql_c_scalar_type left,
                                   ql_c_scalar_type right) {
    if (left.kind == QL_C_SCALAR_FLOAT || right.kind == QL_C_SCALAR_FLOAT) {
        if (left.kind != QL_C_SCALAR_FLOAT) {
            return right;
        }
        if (right.kind != QL_C_SCALAR_FLOAT) {
            return left;
        }
        return left.rank >= right.rank ? left : right;
    }
    if (left.is_signed == right.is_signed) {
        return left.rank >= right.rank ? left : right;
    }
    if (left.is_signed == 0u) {
        ql_c_scalar_type temporary = left;
        left = right;
        right = temporary;
    }
    /* `left` is signed and `right` is unsigned from here. */
    if (right.rank >= left.rank) {
        return right;
    }
    if (left.width > right.width) {
        return left;
    }
    left.is_signed = 0u;
    return left;
}

int ql_c_scalar_can_represent(ql_c_scalar_type type, uint64_t value) {
    uint32_t bits;

    if (type.kind == QL_C_SCALAR_BOOL) {
        return value <= 1u;
    }
    if (type.kind != QL_C_SCALAR_INTEGER || type.width == 0u ||
        type.width > 64u) {
        return 0;
    }
    bits = type.is_signed != 0u ? type.width - 1u : type.width;
    if (bits >= 64u) {
        return 1;
    }
    return value < (UINT64_C(1) << bits);
}
