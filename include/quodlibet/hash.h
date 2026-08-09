#ifndef QUODLIBET_HASH_H
#define QUODLIBET_HASH_H

#include "quodlibet/common.h"

#define QL_DIGEST_SIZE 32u
#define QL_DIGEST_HEX_SIZE 65u

QL_EXTERN_C_BEGIN

typedef struct ql_digest {
    uint8_t bytes[QL_DIGEST_SIZE];
} ql_digest;

QL_API uint64_t QL_CALL ql_fast_hash(const void *data, size_t size,
                                     uint64_t seed);
QL_API void QL_CALL ql_digest_data(const void *data, size_t size,
                                   ql_digest *digest);
QL_API void QL_CALL ql_digest_hex(const ql_digest *digest,
                                  char output[QL_DIGEST_HEX_SIZE]);
QL_API uint32_t QL_CALL ql_digest_equal(const ql_digest *left,
                                        const ql_digest *right);

QL_EXTERN_C_END

#endif
