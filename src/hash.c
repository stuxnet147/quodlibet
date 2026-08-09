#include "quodlibet/hash.h"

#include <string.h>

#include "blake3.h"
#include "xxhash.h"

uint64_t QL_CALL ql_fast_hash(const void *data, size_t size, uint64_t seed) {
    static const uint8_t empty = 0u;
    const void *bytes = data != NULL ? data : &empty;
    if (data == NULL) {
        size = 0u;
    }
    return XXH3_64bits_withSeed(bytes, size, seed);
}

void QL_CALL ql_digest_data(const void *data, size_t size, ql_digest *digest) {
    static const uint8_t empty = 0u;
    const void *bytes = data != NULL ? data : &empty;
    blake3_hasher hasher;

    if (digest == NULL) {
        return;
    }
    if (data == NULL) {
        size = 0u;
    }
    blake3_hasher_init(&hasher);
    blake3_hasher_update(&hasher, bytes, size);
    blake3_hasher_finalize(&hasher, digest->bytes, QL_DIGEST_SIZE);
}

void QL_CALL ql_digest_hex(const ql_digest *digest,
                           char output[QL_DIGEST_HEX_SIZE]) {
    static const char digits[] = "0123456789abcdef";
    size_t index;

    if (output == NULL) {
        return;
    }
    if (digest == NULL) {
        output[0] = '\0';
        return;
    }
    for (index = 0u; index < QL_DIGEST_SIZE; ++index) {
        output[index * 2u] = digits[digest->bytes[index] >> 4u];
        output[index * 2u + 1u] = digits[digest->bytes[index] & 0x0fu];
    }
    output[QL_DIGEST_SIZE * 2u] = '\0';
}

uint32_t QL_CALL ql_digest_equal(const ql_digest *left,
                                 const ql_digest *right) {
    uint8_t difference = 0u;
    size_t index;

    if (left == NULL || right == NULL) {
        return 0u;
    }
    for (index = 0u; index < QL_DIGEST_SIZE; ++index) {
        difference = (uint8_t)(difference |
                               (left->bytes[index] ^ right->bytes[index]));
    }
    return difference == 0u;
}
