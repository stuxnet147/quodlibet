#include "quodlibet/cache.h"

#include <stdio.h>
#include <string.h>

#include "uv.h"

/* On-disk record, little-endian throughout.

     0   magic "QLCACHE\0"
     8   u32 format_version
    12   u32 record_kind
    16   u32 evidence_class
    20   u32 artifact_schema_version
    24   u64 kind_size
    32   u64 method_name_size
    40   u64 method_version_size
    48   u64 options_size
    56   u64 data_size
    64   digest cache_key
    96   digest artifact_digest
   128   digest semantic_problem_digest
   160   digest content_digest
   192   payload: kind, method name, method version, options, data
   end    digest record_digest over everything before it

   content_digest is the BLAKE3 of the artifact bytes and record_digest covers
   the whole encoding. Both are checked on load; a record that fails either is
   refused, not repaired. */

#define QL_CACHE_HEADER_SIZE 192u
#define QL_CACHE_RECORD_ARTIFACT 1u
#define QL_CACHE_RECORD_EVIDENCE 2u
#define QL_CACHE_PATH_CAPACITY 1024u

struct ql_cache {
    ql_allocator allocator;
    char root[QL_CACHE_PATH_CAPACITY];
    uint32_t read_only;
    uint64_t max_entry_bytes;
    ql_cache_stats_v1 stats;
};

static const uint8_t QL_CACHE_MAGIC[8] = {'Q', 'L', 'C', 'A',
                                          'C', 'H', 'E', 0};

static const ql_allocator *resolve_allocator(const ql_allocator *allocator) {
    if (allocator != NULL && ql_allocator_is_valid(allocator)) {
        return allocator;
    }
    return ql_default_allocator();
}

static void write_u32(uint8_t *buffer, uint32_t value) {
    buffer[0] = (uint8_t)(value & 0xFFu);
    buffer[1] = (uint8_t)((value >> 8) & 0xFFu);
    buffer[2] = (uint8_t)((value >> 16) & 0xFFu);
    buffer[3] = (uint8_t)((value >> 24) & 0xFFu);
}

static uint32_t read_u32(const uint8_t *buffer) {
    return (uint32_t)buffer[0] | ((uint32_t)buffer[1] << 8) |
           ((uint32_t)buffer[2] << 16) | ((uint32_t)buffer[3] << 24);
}

static void write_u64(uint8_t *buffer, uint64_t value) {
    write_u32(buffer, (uint32_t)(value & 0xFFFFFFFFu));
    write_u32(buffer + 4u, (uint32_t)(value >> 32));
}

static uint64_t read_u64(const uint8_t *buffer) {
    return (uint64_t)read_u32(buffer) | ((uint64_t)read_u32(buffer + 4u) << 32);
}

/* --- paths ------------------------------------------------------------- */

static ql_status make_directory(const char *path, ql_error *error) {
    uv_fs_t request;
    int result;
    memset(&request, 0, sizeof(request));
    result = uv_fs_mkdir(NULL, &request, path, 0755, NULL);
    uv_fs_req_cleanup(&request);
    if (result == 0 || result == UV_EEXIST) {
        return QL_STATUS_OK;
    }
    ql_error_set(error, QL_STATUS_IO_ERROR,
                 "could not create cache directory '%s': %s", path,
                 uv_strerror(result));
    return QL_STATUS_IO_ERROR;
}

static ql_status build_path(const ql_cache *cache, const ql_digest *key,
                            const char *suffix, char *out, size_t capacity,
                            ql_error *error) {
    char hex[QL_DIGEST_HEX_SIZE];
    int written;
    ql_digest_hex(key, hex);
    written = snprintf(out, capacity, "%s/%c%c/%s%s", cache->root, hex[0],
                       hex[1], hex, suffix);
    if (written < 0 || (size_t)written >= capacity) {
        ql_error_set(error, QL_STATUS_INVALID_ARGUMENT,
                     "the cache path for this key does not fit in %zu bytes",
                     capacity);
        return QL_STATUS_INVALID_ARGUMENT;
    }
    return QL_STATUS_OK;
}

static ql_status ensure_bucket(const ql_cache *cache, const ql_digest *key,
                               ql_error *error) {
    char hex[QL_DIGEST_HEX_SIZE];
    char path[QL_CACHE_PATH_CAPACITY];
    int written;
    ql_digest_hex(key, hex);
    written = snprintf(path, sizeof(path), "%s/%c%c", cache->root, hex[0],
                       hex[1]);
    if (written < 0 || (size_t)written >= sizeof(path)) {
        ql_error_set(error, QL_STATUS_INVALID_ARGUMENT,
                     "the cache bucket path does not fit");
        return QL_STATUS_INVALID_ARGUMENT;
    }
    return make_directory(path, error);
}

/* --- file helpers ------------------------------------------------------ */

static ql_status write_whole_file(const char *path, const uint8_t *bytes,
                                  size_t size, ql_error *error) {
    uv_fs_t request;
    uv_buf_t buffer;
    uv_file file;
    int result;
    size_t offset = 0u;

    memset(&request, 0, sizeof(request));
    result = uv_fs_open(NULL, &request, path,
                        UV_FS_O_WRONLY | UV_FS_O_CREAT | UV_FS_O_TRUNC, 0644,
                        NULL);
    uv_fs_req_cleanup(&request);
    if (result < 0) {
        ql_error_set(error, QL_STATUS_IO_ERROR,
                     "could not open '%s' for writing: %s", path,
                     uv_strerror(result));
        return QL_STATUS_IO_ERROR;
    }
    file = (uv_file)result;
    while (offset < size) {
        buffer = uv_buf_init((char *)(uintptr_t)(bytes + offset),
                             (unsigned int)(size - offset));
        memset(&request, 0, sizeof(request));
        result = uv_fs_write(NULL, &request, file, &buffer, 1u,
                             (int64_t)offset, NULL);
        uv_fs_req_cleanup(&request);
        if (result <= 0) {
            memset(&request, 0, sizeof(request));
            uv_fs_close(NULL, &request, file, NULL);
            uv_fs_req_cleanup(&request);
            ql_error_set(error, QL_STATUS_IO_ERROR,
                         "could not write '%s': %s", path,
                         uv_strerror(result));
            return QL_STATUS_IO_ERROR;
        }
        offset += (size_t)result;
    }
    memset(&request, 0, sizeof(request));
    result = uv_fs_close(NULL, &request, file, NULL);
    uv_fs_req_cleanup(&request);
    if (result < 0) {
        ql_error_set(error, QL_STATUS_IO_ERROR, "could not close '%s': %s",
                     path, uv_strerror(result));
        return QL_STATUS_IO_ERROR;
    }
    return QL_STATUS_OK;
}

static ql_status read_whole_file(const ql_allocator *allocator,
                                 const char *path, uint64_t max_bytes,
                                 uint8_t **out_bytes, size_t *out_size,
                                 ql_error *error) {
    uv_fs_t request;
    uv_file file;
    int result;
    uint64_t size;
    uint8_t *bytes;
    size_t offset = 0u;

    *out_bytes = NULL;
    *out_size = 0u;
    memset(&request, 0, sizeof(request));
    result = uv_fs_open(NULL, &request, path, UV_FS_O_RDONLY, 0, NULL);
    uv_fs_req_cleanup(&request);
    if (result == UV_ENOENT) {
        return QL_STATUS_NOT_FOUND;
    }
    if (result < 0) {
        ql_error_set(error, QL_STATUS_IO_ERROR, "could not open '%s': %s",
                     path, uv_strerror(result));
        return QL_STATUS_IO_ERROR;
    }
    file = (uv_file)result;

    memset(&request, 0, sizeof(request));
    result = uv_fs_fstat(NULL, &request, file, NULL);
    if (result < 0) {
        uv_fs_req_cleanup(&request);
        memset(&request, 0, sizeof(request));
        uv_fs_close(NULL, &request, file, NULL);
        uv_fs_req_cleanup(&request);
        ql_error_set(error, QL_STATUS_IO_ERROR, "could not stat '%s': %s",
                     path, uv_strerror(result));
        return QL_STATUS_IO_ERROR;
    }
    size = request.statbuf.st_size;
    uv_fs_req_cleanup(&request);

    if (size < (uint64_t)(QL_CACHE_HEADER_SIZE + QL_DIGEST_SIZE) ||
        (max_bytes != 0u && size > max_bytes)) {
        memset(&request, 0, sizeof(request));
        uv_fs_close(NULL, &request, file, NULL);
        uv_fs_req_cleanup(&request);
        ql_error_set(error, QL_STATUS_PARSE_ERROR,
                     "cache record '%s' has an unusable size of %llu bytes",
                     path, (unsigned long long)size);
        return QL_STATUS_PARSE_ERROR;
    }
    bytes = allocator->allocate(allocator->user_data, (size_t)size);
    if (bytes == NULL) {
        memset(&request, 0, sizeof(request));
        uv_fs_close(NULL, &request, file, NULL);
        uv_fs_req_cleanup(&request);
        ql_error_set(error, QL_STATUS_OUT_OF_MEMORY,
                     "could not allocate %llu bytes for a cache record",
                     (unsigned long long)size);
        return QL_STATUS_OUT_OF_MEMORY;
    }
    while (offset < (size_t)size) {
        uv_buf_t buffer = uv_buf_init((char *)(bytes + offset),
                                      (unsigned int)((size_t)size - offset));
        memset(&request, 0, sizeof(request));
        result = uv_fs_read(NULL, &request, file, &buffer, 1u,
                            (int64_t)offset, NULL);
        uv_fs_req_cleanup(&request);
        if (result <= 0) {
            allocator->deallocate(allocator->user_data, bytes);
            memset(&request, 0, sizeof(request));
            uv_fs_close(NULL, &request, file, NULL);
            uv_fs_req_cleanup(&request);
            ql_error_set(error, QL_STATUS_IO_ERROR,
                         "could not read '%s': %s", path,
                         uv_strerror(result));
            return QL_STATUS_IO_ERROR;
        }
        offset += (size_t)result;
    }
    memset(&request, 0, sizeof(request));
    uv_fs_close(NULL, &request, file, NULL);
    uv_fs_req_cleanup(&request);
    *out_bytes = bytes;
    *out_size = (size_t)size;
    return QL_STATUS_OK;
}

/* --- record encoding --------------------------------------------------- */

typedef struct ql_cache_record {
    uint32_t record_kind;
    uint32_t evidence_class;
    uint32_t artifact_schema_version;
    ql_digest cache_key;
    ql_digest artifact_digest;
    ql_digest semantic_problem_digest;
    ql_digest content_digest;
    const char *kind;
    size_t kind_size;
    const char *method_name;
    size_t method_name_size;
    const char *method_version;
    size_t method_version_size;
    const void *options;
    size_t options_size;
    const void *data;
    size_t data_size;
} ql_cache_record;

static ql_status encode_record(const ql_allocator *allocator,
                               const ql_cache_record *record,
                               uint8_t **out_bytes, size_t *out_size,
                               ql_error *error) {
    const size_t payload = record->kind_size + record->method_name_size +
                           record->method_version_size +
                           record->options_size + record->data_size;
    const size_t total =
        (size_t)QL_CACHE_HEADER_SIZE + payload + QL_DIGEST_SIZE;
    uint8_t *bytes = allocator->allocate(allocator->user_data, total);
    size_t offset = QL_CACHE_HEADER_SIZE;
    ql_digest record_digest;

    if (bytes == NULL) {
        ql_error_set(error, QL_STATUS_OUT_OF_MEMORY,
                     "could not allocate a cache record of %zu bytes", total);
        return QL_STATUS_OUT_OF_MEMORY;
    }
    memset(bytes, 0, QL_CACHE_HEADER_SIZE);
    memcpy(bytes, QL_CACHE_MAGIC, sizeof(QL_CACHE_MAGIC));
    write_u32(bytes + 8u, QL_CACHE_FORMAT_VERSION);
    write_u32(bytes + 12u, record->record_kind);
    write_u32(bytes + 16u, record->evidence_class);
    write_u32(bytes + 20u, record->artifact_schema_version);
    write_u64(bytes + 24u, (uint64_t)record->kind_size);
    write_u64(bytes + 32u, (uint64_t)record->method_name_size);
    write_u64(bytes + 40u, (uint64_t)record->method_version_size);
    write_u64(bytes + 48u, (uint64_t)record->options_size);
    write_u64(bytes + 56u, (uint64_t)record->data_size);
    memcpy(bytes + 64u, record->cache_key.bytes, QL_DIGEST_SIZE);
    memcpy(bytes + 96u, record->artifact_digest.bytes, QL_DIGEST_SIZE);
    memcpy(bytes + 128u, record->semantic_problem_digest.bytes,
           QL_DIGEST_SIZE);
    memcpy(bytes + 160u, record->content_digest.bytes, QL_DIGEST_SIZE);

    if (record->kind_size != 0u) {
        memcpy(bytes + offset, record->kind, record->kind_size);
        offset += record->kind_size;
    }
    if (record->method_name_size != 0u) {
        memcpy(bytes + offset, record->method_name, record->method_name_size);
        offset += record->method_name_size;
    }
    if (record->method_version_size != 0u) {
        memcpy(bytes + offset, record->method_version,
               record->method_version_size);
        offset += record->method_version_size;
    }
    if (record->options_size != 0u) {
        memcpy(bytes + offset, record->options, record->options_size);
        offset += record->options_size;
    }
    if (record->data_size != 0u) {
        memcpy(bytes + offset, record->data, record->data_size);
        offset += record->data_size;
    }
    ql_digest_data(bytes, offset, &record_digest);
    memcpy(bytes + offset, record_digest.bytes, QL_DIGEST_SIZE);

    *out_bytes = bytes;
    *out_size = total;
    return QL_STATUS_OK;
}

/* Refuses rather than repairs. A record that does not verify is not a cache
   miss either: silently recomputing would hide a store that is handing out
   the wrong answer. */
static ql_status decode_record(const uint8_t *bytes, size_t size,
                               const ql_digest *expected_key,
                               ql_cache_record *record, ql_error *error) {
    size_t offset = QL_CACHE_HEADER_SIZE;
    size_t payload;
    ql_digest computed;
    ql_digest stored;

    if (size < (size_t)QL_CACHE_HEADER_SIZE + QL_DIGEST_SIZE ||
        memcmp(bytes, QL_CACHE_MAGIC, sizeof(QL_CACHE_MAGIC)) != 0) {
        ql_error_set(error, QL_STATUS_PARSE_ERROR,
                     "the file is not a quodlibet cache record");
        return QL_STATUS_PARSE_ERROR;
    }
    if (read_u32(bytes + 8u) != QL_CACHE_FORMAT_VERSION) {
        ql_error_set(error, QL_STATUS_SCHEMA_MISMATCH,
                     "cache record format version is %u, this build reads %u",
                     read_u32(bytes + 8u), (unsigned)QL_CACHE_FORMAT_VERSION);
        return QL_STATUS_SCHEMA_MISMATCH;
    }
    memset(record, 0, sizeof(*record));
    record->record_kind = read_u32(bytes + 12u);
    record->evidence_class = read_u32(bytes + 16u);
    record->artifact_schema_version = read_u32(bytes + 20u);
    record->kind_size = (size_t)read_u64(bytes + 24u);
    record->method_name_size = (size_t)read_u64(bytes + 32u);
    record->method_version_size = (size_t)read_u64(bytes + 40u);
    record->options_size = (size_t)read_u64(bytes + 48u);
    record->data_size = (size_t)read_u64(bytes + 56u);
    memcpy(record->cache_key.bytes, bytes + 64u, QL_DIGEST_SIZE);
    memcpy(record->artifact_digest.bytes, bytes + 96u, QL_DIGEST_SIZE);
    memcpy(record->semantic_problem_digest.bytes, bytes + 128u,
           QL_DIGEST_SIZE);
    memcpy(record->content_digest.bytes, bytes + 160u, QL_DIGEST_SIZE);

    payload = record->kind_size + record->method_name_size +
              record->method_version_size + record->options_size +
              record->data_size;
    if (payload > size - QL_CACHE_HEADER_SIZE - QL_DIGEST_SIZE ||
        (size_t)QL_CACHE_HEADER_SIZE + payload + QL_DIGEST_SIZE != size) {
        ql_error_set(error, QL_STATUS_PARSE_ERROR,
                     "cache record lengths do not add up to its file size");
        return QL_STATUS_PARSE_ERROR;
    }
    ql_digest_data(bytes, QL_CACHE_HEADER_SIZE + payload, &computed);
    memcpy(stored.bytes, bytes + QL_CACHE_HEADER_SIZE + payload,
           QL_DIGEST_SIZE);
    if (!ql_digest_equal(&computed, &stored)) {
        ql_error_set(error, QL_STATUS_IO_ERROR,
                     "cache record digest does not match its contents");
        return QL_STATUS_IO_ERROR;
    }
    if (expected_key != NULL &&
        !ql_digest_equal(expected_key, &record->cache_key)) {
        ql_error_set(error, QL_STATUS_IO_ERROR,
                     "cache record was stored under a different key than the "
                     "one being looked up");
        return QL_STATUS_IO_ERROR;
    }

    record->kind = (const char *)(bytes + offset);
    offset += record->kind_size;
    record->method_name = (const char *)(bytes + offset);
    offset += record->method_name_size;
    record->method_version = (const char *)(bytes + offset);
    offset += record->method_version_size;
    record->options = bytes + offset;
    offset += record->options_size;
    record->data = bytes + offset;

    ql_digest_data(record->data_size == 0u ? NULL : record->data,
                   record->data_size, &computed);
    if (!ql_digest_equal(&computed, &record->content_digest)) {
        ql_error_set(error, QL_STATUS_IO_ERROR,
                     "cache record content does not match its BLAKE3 "
                     "identity");
        return QL_STATUS_IO_ERROR;
    }
    return QL_STATUS_OK;
}

/* --- lifecycle --------------------------------------------------------- */

void QL_CALL ql_cache_config_init(ql_cache_config_v1 *config) {
    if (config == NULL) {
        return;
    }
    memset(config, 0, sizeof(*config));
    config->struct_size = sizeof(*config);
    config->abi_version = QL_ABI_VERSION;
    config->format_version = QL_CACHE_FORMAT_VERSION;
    config->create_if_missing = 1u;
}

ql_status QL_CALL ql_cache_open(const ql_allocator *allocator,
                                const ql_cache_config_v1 *config,
                                ql_cache **cache, ql_error *error) {
    const ql_allocator *actual = resolve_allocator(allocator);
    ql_cache *created;
    int written;

    if (cache == NULL) {
        ql_error_set(error, QL_STATUS_INVALID_ARGUMENT,
                     "a cache output is required");
        return QL_STATUS_INVALID_ARGUMENT;
    }
    *cache = NULL;
    if (config == NULL || config->struct_size != sizeof(*config)) {
        ql_error_set(error, QL_STATUS_ABI_MISMATCH,
                     "cache config struct_size is %zu, expected %zu",
                     config == NULL ? (size_t)0 : config->struct_size,
                     sizeof(*config));
        return QL_STATUS_ABI_MISMATCH;
    }
    if (config->abi_version != QL_ABI_VERSION) {
        ql_error_set(error, QL_STATUS_ABI_MISMATCH,
                     "cache config abi_version is %u, expected %u",
                     config->abi_version, (unsigned)QL_ABI_VERSION);
        return QL_STATUS_ABI_MISMATCH;
    }
    if (config->format_version != QL_CACHE_FORMAT_VERSION) {
        ql_error_set(error, QL_STATUS_SCHEMA_MISMATCH,
                     "cache format version is %u, this build writes %u",
                     config->format_version,
                     (unsigned)QL_CACHE_FORMAT_VERSION);
        return QL_STATUS_SCHEMA_MISMATCH;
    }
    if (config->root_path == NULL || config->root_path[0] == '\0') {
        ql_error_set(error, QL_STATUS_INVALID_ARGUMENT,
                     "the caller must name the cache root directory");
        return QL_STATUS_INVALID_ARGUMENT;
    }

    created = actual->allocate(actual->user_data, sizeof(*created));
    if (created == NULL) {
        ql_error_set(error, QL_STATUS_OUT_OF_MEMORY,
                     "could not allocate the cache handle");
        return QL_STATUS_OUT_OF_MEMORY;
    }
    memset(created, 0, sizeof(*created));
    created->allocator = *actual;
    created->read_only = config->read_only;
    created->max_entry_bytes = config->max_entry_bytes;
    created->stats.struct_size = sizeof(created->stats);
    written = snprintf(created->root, sizeof(created->root), "%s/v%u",
                       config->root_path,
                       (unsigned)QL_CACHE_FORMAT_VERSION);
    if (written < 0 || (size_t)written >= sizeof(created->root)) {
        actual->deallocate(actual->user_data, created);
        ql_error_set(error, QL_STATUS_INVALID_ARGUMENT,
                     "the cache root path is too long");
        return QL_STATUS_INVALID_ARGUMENT;
    }
    if (config->create_if_missing != 0u && config->read_only == 0u) {
        if (make_directory(config->root_path, error) != QL_STATUS_OK ||
            make_directory(created->root, error) != QL_STATUS_OK) {
            actual->deallocate(actual->user_data, created);
            return QL_STATUS_IO_ERROR;
        }
    }
    *cache = created;
    ql_error_clear(error);
    return QL_STATUS_OK;
}

void QL_CALL ql_cache_close(ql_cache *cache) {
    ql_allocator allocator;
    if (cache == NULL) {
        return;
    }
    allocator = cache->allocator;
    allocator.deallocate(allocator.user_data, cache);
}

/* --- store and load ---------------------------------------------------- */

static ql_status store_record(ql_cache *cache, const ql_digest *key,
                              const ql_cache_record *record,
                              ql_error *error) {
    char path[QL_CACHE_PATH_CAPACITY];
    uint8_t *bytes = NULL;
    size_t size = 0u;
    uint8_t *existing = NULL;
    size_t existing_size = 0u;
    ql_status status;

    if (cache->read_only != 0u) {
        ql_error_set(error, QL_STATUS_IO_ERROR,
                     "the cache was opened read-only");
        return QL_STATUS_IO_ERROR;
    }
    status = build_path(cache, key, ".qlc", path, sizeof(path), error);
    if (status != QL_STATUS_OK) {
        return status;
    }
    status = encode_record(&cache->allocator, record, &bytes, &size, error);
    if (status != QL_STATUS_OK) {
        return status;
    }
    if (cache->max_entry_bytes != 0u &&
        (uint64_t)size > cache->max_entry_bytes) {
        cache->allocator.deallocate(cache->allocator.user_data, bytes);
        ql_error_set(error, QL_STATUS_INVALID_ARGUMENT,
                     "the record is %zu bytes and the cache limit is %llu",
                     size, (unsigned long long)cache->max_entry_bytes);
        return QL_STATUS_INVALID_ARGUMENT;
    }

    /* An existing entry is only acceptable when it is byte-identical. Two
       different answers filed under one key means the key is missing an axis,
       and overwriting would turn that defect into a silent wrong answer. */
    status = read_whole_file(&cache->allocator, path, 0u, &existing,
                             &existing_size, error);
    if (status == QL_STATUS_OK) {
        const int identical = existing_size == size &&
                              memcmp(existing, bytes, size) == 0;
        cache->allocator.deallocate(cache->allocator.user_data, existing);
        cache->allocator.deallocate(cache->allocator.user_data, bytes);
        if (identical) {
            ql_error_clear(error);
            return QL_STATUS_OK;
        }
        ql_error_set(error, QL_STATUS_ALREADY_EXISTS,
                     "this cache key already holds different content, so the "
                     "key does not identify the question it answers");
        return QL_STATUS_ALREADY_EXISTS;
    }
    if (status != QL_STATUS_NOT_FOUND) {
        cache->allocator.deallocate(cache->allocator.user_data, bytes);
        ++cache->stats.rejected_records;
        return status;
    }

    status = ensure_bucket(cache, key, error);
    if (status == QL_STATUS_OK) {
        status = write_whole_file(path, bytes, size, error);
    }
    cache->allocator.deallocate(cache->allocator.user_data, bytes);
    if (status == QL_STATUS_OK) {
        ++cache->stats.stores;
        cache->stats.bytes_written += (uint64_t)size;
        ql_error_clear(error);
    }
    return status;
}

static ql_status load_record(ql_cache *cache, const ql_digest *key,
                             uint32_t expected_kind, uint8_t **out_bytes,
                             ql_cache_record *record, ql_error *error) {
    char path[QL_CACHE_PATH_CAPACITY];
    uint8_t *bytes = NULL;
    size_t size = 0u;
    ql_status status;

    *out_bytes = NULL;
    status = build_path(cache, key, ".qlc", path, sizeof(path), error);
    if (status != QL_STATUS_OK) {
        return status;
    }
    status = read_whole_file(&cache->allocator, path,
                             cache->max_entry_bytes, &bytes, &size, error);
    if (status == QL_STATUS_NOT_FOUND) {
        ++cache->stats.misses;
        ql_error_set(error, QL_STATUS_NOT_FOUND,
                     "no cache record is stored under this key");
        return QL_STATUS_NOT_FOUND;
    }
    if (status != QL_STATUS_OK) {
        ++cache->stats.rejected_records;
        return status;
    }
    status = decode_record(bytes, size, key, record, error);
    if (status != QL_STATUS_OK) {
        cache->allocator.deallocate(cache->allocator.user_data, bytes);
        ++cache->stats.rejected_records;
        return status;
    }
    if (record->record_kind != expected_kind) {
        cache->allocator.deallocate(cache->allocator.user_data, bytes);
        ++cache->stats.rejected_records;
        ql_error_set(error, QL_STATUS_TYPE_MISMATCH,
                     "the record under this key is kind %u, not %u",
                     record->record_kind, expected_kind);
        return QL_STATUS_TYPE_MISMATCH;
    }
    ++cache->stats.hits;
    cache->stats.bytes_read += (uint64_t)size;
    *out_bytes = bytes;
    return QL_STATUS_OK;
}

ql_status QL_CALL ql_cache_store_artifact(ql_cache *cache,
                                          const ql_digest *key,
                                          const ql_artifact *artifact,
                                          ql_error *error) {
    ql_artifact_view artifact_view;
    ql_cache_record record;
    ql_status status;

    if (cache == NULL || key == NULL || artifact == NULL) {
        ql_error_set(error, QL_STATUS_INVALID_ARGUMENT,
                     "a cache, a key, and an artifact are required");
        return QL_STATUS_INVALID_ARGUMENT;
    }
    memset(&artifact_view, 0, sizeof(artifact_view));
    status = ql_artifact_get_view(artifact, &artifact_view, error);
    if (status != QL_STATUS_OK) {
        return status;
    }
    memset(&record, 0, sizeof(record));
    record.record_kind = QL_CACHE_RECORD_ARTIFACT;
    record.artifact_schema_version = artifact_view.schema_version;
    record.cache_key = *key;
    ql_digest_data(artifact_view.data, artifact_view.size,
                   &record.content_digest);
    record.kind = artifact_view.kind;
    record.kind_size = artifact_view.kind == NULL
                           ? 0u
                           : strlen(artifact_view.kind);
    record.data = artifact_view.data;
    record.data_size = artifact_view.size;
    return store_record(cache, key, &record, error);
}

static ql_status artifact_from_record(ql_cache *cache,
                                      const ql_cache_record *record,
                                      ql_artifact **artifact,
                                      ql_error *error) {
    char kind[128];
    if (record->kind_size >= sizeof(kind)) {
        ql_error_set(error, QL_STATUS_PARSE_ERROR,
                     "the record's artifact kind is %zu bytes",
                     record->kind_size);
        return QL_STATUS_PARSE_ERROR;
    }
    memcpy(kind, record->kind, record->kind_size);
    kind[record->kind_size] = '\0';
    return ql_artifact_create(&cache->allocator, kind,
                              record->artifact_schema_version, record->data,
                              record->data_size, artifact, error);
}

ql_status QL_CALL ql_cache_load_artifact(ql_cache *cache,
                                         const ql_digest *key,
                                         ql_artifact **artifact,
                                         ql_error *error) {
    ql_cache_record record;
    uint8_t *bytes = NULL;
    ql_status status;

    if (cache == NULL || key == NULL || artifact == NULL) {
        ql_error_set(error, QL_STATUS_INVALID_ARGUMENT,
                     "a cache, a key, and an artifact output are required");
        return QL_STATUS_INVALID_ARGUMENT;
    }
    *artifact = NULL;
    status = load_record(cache, key, QL_CACHE_RECORD_ARTIFACT, &bytes,
                         &record, error);
    if (status != QL_STATUS_OK) {
        return status;
    }
    status = artifact_from_record(cache, &record, artifact, error);
    cache->allocator.deallocate(cache->allocator.user_data, bytes);
    return status;
}

ql_status QL_CALL ql_cache_store_evidence(
    ql_cache *cache, const ql_evidence_envelope *envelope,
    const void *canonical_options, size_t canonical_options_size,
    ql_error *error) {
    ql_evidence_envelope_view_v1 envelope_view;
    ql_artifact_view artifact_view;
    ql_cache_record record;
    ql_status status;

    if (cache == NULL || envelope == NULL) {
        ql_error_set(error, QL_STATUS_INVALID_ARGUMENT,
                     "a cache and an evidence envelope are required");
        return QL_STATUS_INVALID_ARGUMENT;
    }
    memset(&envelope_view, 0, sizeof(envelope_view));
    status = ql_evidence_envelope_get_view(envelope, &envelope_view, error);
    if (status != QL_STATUS_OK) {
        return status;
    }
    if (envelope_view.evidence_artifact == NULL) {
        ql_error_set(error, QL_STATUS_INVALID_ARGUMENT,
                     "only an envelope carrying an evidence artifact can be "
                     "stored");
        return QL_STATUS_INVALID_ARGUMENT;
    }
    memset(&artifact_view, 0, sizeof(artifact_view));
    status = ql_artifact_get_view(envelope_view.evidence_artifact,
                                  &artifact_view, error);
    if (status != QL_STATUS_OK) {
        return status;
    }
    memset(&record, 0, sizeof(record));
    record.record_kind = QL_CACHE_RECORD_EVIDENCE;
    record.evidence_class = (uint32_t)envelope_view.evidence_class;
    record.artifact_schema_version = artifact_view.schema_version;
    record.cache_key = envelope_view.cache_key;
    record.artifact_digest = envelope_view.artifact_digest;
    record.semantic_problem_digest = envelope_view.semantic_problem_digest;
    ql_digest_data(artifact_view.data, artifact_view.size,
                   &record.content_digest);
    record.kind = artifact_view.kind;
    record.kind_size =
        artifact_view.kind == NULL ? 0u : strlen(artifact_view.kind);
    record.method_name = envelope_view.method_name;
    record.method_name_size = envelope_view.method_name == NULL
                                  ? 0u
                                  : strlen(envelope_view.method_name);
    record.method_version = envelope_view.method_version;
    record.method_version_size = envelope_view.method_version == NULL
                                     ? 0u
                                     : strlen(envelope_view.method_version);
    record.options = canonical_options;
    record.options_size = canonical_options == NULL
                              ? 0u
                              : canonical_options_size;
    record.data = artifact_view.data;
    record.data_size = artifact_view.size;
    return store_record(cache, &envelope_view.cache_key, &record, error);
}

ql_status QL_CALL ql_cache_load_evidence(ql_cache *cache,
                                         const ql_digest *key,
                                         ql_evidence_envelope **envelope,
                                         ql_error *error) {
    ql_cache_record record;
    ql_cache_key_input_v1 identity;
    ql_artifact *artifact = NULL;
    uint8_t *bytes = NULL;
    char *method_name = NULL;
    char *method_version = NULL;
    ql_digest recomputed;
    ql_status status;

    if (cache == NULL || key == NULL || envelope == NULL) {
        ql_error_set(error, QL_STATUS_INVALID_ARGUMENT,
                     "a cache, a key, and an envelope output are required");
        return QL_STATUS_INVALID_ARGUMENT;
    }
    *envelope = NULL;
    status = load_record(cache, key, QL_CACHE_RECORD_EVIDENCE, &bytes,
                         &record, error);
    if (status != QL_STATUS_OK) {
        return status;
    }

    method_name = cache->allocator.allocate(cache->allocator.user_data,
                                            record.method_name_size + 1u);
    method_version = cache->allocator.allocate(
        cache->allocator.user_data, record.method_version_size + 1u);
    if (method_name == NULL || method_version == NULL) {
        cache->allocator.deallocate(cache->allocator.user_data, method_name);
        cache->allocator.deallocate(cache->allocator.user_data,
                                    method_version);
        cache->allocator.deallocate(cache->allocator.user_data, bytes);
        ql_error_set(error, QL_STATUS_OUT_OF_MEMORY,
                     "could not allocate the record's method identity");
        return QL_STATUS_OUT_OF_MEMORY;
    }
    memcpy(method_name, record.method_name, record.method_name_size);
    method_name[record.method_name_size] = '\0';
    memcpy(method_version, record.method_version, record.method_version_size);
    method_version[record.method_version_size] = '\0';

    memset(&identity, 0, sizeof(identity));
    identity.struct_size = sizeof(identity);
    identity.artifact_digest = record.artifact_digest;
    identity.semantic_problem_digest = record.semantic_problem_digest;
    identity.method_name = method_name;
    identity.method_version = method_version;
    identity.canonical_options = record.options_size == 0u ? NULL
                                                           : record.options;
    identity.canonical_options_size = record.options_size;

    /* The stored key is not taken on faith. Recomputing it from the stored
       identity is what makes a record that was filed under the wrong key
       unusable instead of authoritative. */
    status = ql_cache_key_compute(&identity, &recomputed, error);
    if (status == QL_STATUS_OK && !ql_digest_equal(&recomputed, key)) {
        ql_error_set(error, QL_STATUS_IO_ERROR,
                     "the record's identity does not recompute to the key it "
                     "is filed under");
        status = QL_STATUS_IO_ERROR;
    }
    if (status == QL_STATUS_OK) {
        status = artifact_from_record(cache, &record, &artifact, error);
    }
    if (status == QL_STATUS_OK) {
        status = ql_evidence_envelope_create(
            &cache->allocator, &identity,
            (ql_evidence_class)record.evidence_class, artifact, envelope,
            error);
        ql_artifact_release(artifact);
    }
    if (status != QL_STATUS_OK) {
        ++cache->stats.rejected_records;
    }
    cache->allocator.deallocate(cache->allocator.user_data, method_name);
    cache->allocator.deallocate(cache->allocator.user_data, method_version);
    cache->allocator.deallocate(cache->allocator.user_data, bytes);
    return status;
}

ql_status QL_CALL ql_cache_contains(ql_cache *cache, const ql_digest *key,
                                    uint32_t *present, ql_error *error) {
    char path[QL_CACHE_PATH_CAPACITY];
    uv_fs_t request;
    int result;
    ql_status status;

    if (cache == NULL || key == NULL || present == NULL) {
        ql_error_set(error, QL_STATUS_INVALID_ARGUMENT,
                     "a cache, a key, and an output are required");
        return QL_STATUS_INVALID_ARGUMENT;
    }
    *present = 0u;
    status = build_path(cache, key, ".qlc", path, sizeof(path), error);
    if (status != QL_STATUS_OK) {
        return status;
    }
    memset(&request, 0, sizeof(request));
    result = uv_fs_stat(NULL, &request, path, NULL);
    uv_fs_req_cleanup(&request);
    if (result == 0) {
        *present = 1u;
    } else if (result != UV_ENOENT) {
        ql_error_set(error, QL_STATUS_IO_ERROR,
                     "could not stat the cache record: %s",
                     uv_strerror(result));
        return QL_STATUS_IO_ERROR;
    }
    ql_error_clear(error);
    return QL_STATUS_OK;
}

ql_status QL_CALL ql_cache_remove(ql_cache *cache, const ql_digest *key,
                                  ql_error *error) {
    char path[QL_CACHE_PATH_CAPACITY];
    uv_fs_t request;
    int result;
    ql_status status;

    if (cache == NULL || key == NULL) {
        ql_error_set(error, QL_STATUS_INVALID_ARGUMENT,
                     "a cache and a key are required");
        return QL_STATUS_INVALID_ARGUMENT;
    }
    if (cache->read_only != 0u) {
        ql_error_set(error, QL_STATUS_IO_ERROR,
                     "the cache was opened read-only");
        return QL_STATUS_IO_ERROR;
    }
    status = build_path(cache, key, ".qlc", path, sizeof(path), error);
    if (status != QL_STATUS_OK) {
        return status;
    }
    memset(&request, 0, sizeof(request));
    result = uv_fs_unlink(NULL, &request, path, NULL);
    uv_fs_req_cleanup(&request);
    if (result == UV_ENOENT) {
        ql_error_set(error, QL_STATUS_NOT_FOUND,
                     "no cache record is stored under this key");
        return QL_STATUS_NOT_FOUND;
    }
    if (result < 0) {
        ql_error_set(error, QL_STATUS_IO_ERROR,
                     "could not remove the cache record: %s",
                     uv_strerror(result));
        return QL_STATUS_IO_ERROR;
    }
    ql_error_clear(error);
    return QL_STATUS_OK;
}

ql_status QL_CALL ql_cache_get_stats(const ql_cache *cache,
                                     ql_cache_stats_v1 *stats,
                                     ql_error *error) {
    if (cache == NULL || stats == NULL) {
        ql_error_set(error, QL_STATUS_INVALID_ARGUMENT,
                     "a cache and a stats output are required");
        return QL_STATUS_INVALID_ARGUMENT;
    }
    *stats = cache->stats;
    stats->struct_size = sizeof(*stats);
    ql_error_clear(error);
    return QL_STATUS_OK;
}
