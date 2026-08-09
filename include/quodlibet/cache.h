#ifndef QUODLIBET_CACHE_H
#define QUODLIBET_CACHE_H

#include "quodlibet/evidence.h"

QL_EXTERN_C_BEGIN

/* A persistent store for artifacts and evidence envelopes, addressed by the
   BLAKE3 cache key. The location is the caller's decision; this module never
   picks a directory of its own.

   Every record carries the digest of its own content and a digest over its
   whole encoding. Both are verified on load, so a truncated, corrupted, or
   substituted file is an error and never a silently wrong answer. The cache
   key a record was stored under is written into the record and compared with
   the key being looked up, so a misplaced file cannot answer for another
   question either.

   What the key must contain is not this module's decision. ql_cache_key_compute
   owns that, and a key that omits an axis will happily store and return the
   wrong answer here; see the cache-key completeness tests. */

#define QL_CACHE_FORMAT_VERSION 1u

typedef struct ql_cache ql_cache;

typedef struct ql_cache_config_v1 {
    size_t struct_size;
    uint32_t abi_version;
    uint32_t format_version;
    /* Required. The cache lives under this directory and creates a
       version-named subdirectory inside it. */
    const char *root_path;
    uint32_t create_if_missing;
    uint32_t read_only;
    /* Refuse to store or load anything larger. Zero means no limit. */
    uint64_t max_entry_bytes;
    uint64_t reserved[4];
} ql_cache_config_v1;

typedef struct ql_cache_stats_v1 {
    size_t struct_size;
    uint64_t stores;
    uint64_t hits;
    uint64_t misses;
    uint64_t rejected_records;
    uint64_t bytes_written;
    uint64_t bytes_read;
    uint64_t reserved[4];
} ql_cache_stats_v1;

QL_API void QL_CALL ql_cache_config_init(ql_cache_config_v1 *config);

QL_API ql_status QL_CALL ql_cache_open(const ql_allocator *allocator,
                                       const ql_cache_config_v1 *config,
                                       ql_cache **cache, ql_error *error);
QL_API void QL_CALL ql_cache_close(ql_cache *cache);

/* Storing the same key twice with the same content is a no-op and succeeds.
   Storing a different artifact under a key that already holds one is
   ALREADY_EXISTS: two different answers to one question means the key is
   missing an axis, and overwriting would hide that. */
QL_API ql_status QL_CALL ql_cache_store_artifact(ql_cache *cache,
                                                 const ql_digest *key,
                                                 const ql_artifact *artifact,
                                                 ql_error *error);
/* NOT_FOUND on a miss. The caller owns the returned artifact. */
QL_API ql_status QL_CALL ql_cache_load_artifact(ql_cache *cache,
                                                const ql_digest *key,
                                                ql_artifact **artifact,
                                                ql_error *error);

/* Envelope records additionally carry the identity the envelope was built
   from, so the loaded envelope recomputes the same cache key rather than
   trusting the stored one. The options bytes are the method-defined canonical
   encoding and may be null when there are none. */
QL_API ql_status QL_CALL ql_cache_store_evidence(
    ql_cache *cache, const ql_evidence_envelope *envelope,
    const void *canonical_options, size_t canonical_options_size,
    ql_error *error);
QL_API ql_status QL_CALL ql_cache_load_evidence(
    ql_cache *cache, const ql_digest *key, ql_evidence_envelope **envelope,
    ql_error *error);

QL_API ql_status QL_CALL ql_cache_contains(ql_cache *cache,
                                           const ql_digest *key,
                                           uint32_t *present,
                                           ql_error *error);
QL_API ql_status QL_CALL ql_cache_remove(ql_cache *cache,
                                         const ql_digest *key,
                                         ql_error *error);
QL_API ql_status QL_CALL ql_cache_get_stats(const ql_cache *cache,
                                            ql_cache_stats_v1 *stats,
                                            ql_error *error);

QL_EXTERN_C_END

#endif
