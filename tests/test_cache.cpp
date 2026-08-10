#include "quodlibet/cache.h"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <vector>

#if defined(_WIN32)
#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <unistd.h>
#endif

#include <gtest/gtest.h>

namespace {

namespace fs = std::filesystem;

struct CacheDeleter {
    void operator()(ql_cache *cache) const { ql_cache_close(cache); }
};

struct ArtifactDeleter {
    void operator()(ql_artifact *artifact) const {
        ql_artifact_release(artifact);
    }
};

struct EnvelopeDeleter {
    void operator()(ql_evidence_envelope *envelope) const {
        ql_evidence_envelope_release(envelope);
    }
};

using CachePtr = std::unique_ptr<ql_cache, CacheDeleter>;
using ArtifactPtr = std::unique_ptr<ql_artifact, ArtifactDeleter>;
using EnvelopePtr = std::unique_ptr<ql_evidence_envelope, EnvelopeDeleter>;

/* gtest_discover_tests runs every test in its own process, so a counter with
   static storage duration restarts at one in each of them and every process
   picks the same directory name. Under a parallel ctest one test's constructor
   then deletes the store another test is in the middle of using, and the
   failure surfaces as an unrelated cache miss in whichever test lost the race.
   The process id is what actually distinguishes them; the counter only has to
   separate roots inside one process.

   An address is not a substitute: two identical processes running the same
   test lay out their stacks the same way and hand out the same pointer. */
unsigned long long current_process_id() {
#if defined(_WIN32)
    return static_cast<unsigned long long>(GetCurrentProcessId());
#else
    return static_cast<unsigned long long>(getpid());
#endif
}

/* A cache root that goes away with the test, so a failing run never leaves a
   poisoned store behind for the next one. */
class ScopedRoot {
public:
    ScopedRoot() {
        static int counter = 0;
        path_ = fs::temp_directory_path() /
                ("quodlibet-cache-test-" + std::to_string(current_process_id()) +
                 "-" + std::to_string(++counter));
        std::error_code ignored;
        fs::remove_all(path_, ignored);
    }
    ~ScopedRoot() {
        std::error_code ignored;
        fs::remove_all(path_, ignored);
    }
    std::string string() const { return path_.string(); }
    const fs::path &path() const { return path_; }

private:
    fs::path path_;
};

CachePtr open_cache(const ScopedRoot &root, bool read_only = false) {
    ql_cache_config_v1 config{};
    ql_cache_config_init(&config);
    const std::string text = root.string();
    config.root_path = text.c_str();
    config.read_only = read_only ? 1u : 0u;
    ql_cache *raw = nullptr;
    ql_error error{};
    EXPECT_EQ(QL_STATUS_OK,
              ql_cache_open(nullptr, &config, &raw, &error))
        << error.message;
    return CachePtr(raw);
}

ql_digest key_of(const char *text) {
    ql_digest digest{};
    ql_digest_data(text, std::strlen(text), &digest);
    return digest;
}

ArtifactPtr make_artifact(const char *kind, const char *payload) {
    ql_artifact *raw = nullptr;
    ql_error error{};
    EXPECT_EQ(QL_STATUS_OK,
              ql_artifact_create(nullptr, kind, 1u, payload,
                                 std::strlen(payload), &raw, &error))
        << error.message;
    return ArtifactPtr(raw);
}

std::string payload_of(const ql_artifact *artifact) {
    ql_artifact_view view{};
    ql_error error{};
    EXPECT_EQ(QL_STATUS_OK,
              ql_artifact_get_view(artifact, &view, &error))
        << error.message;
    return std::string(static_cast<const char *>(view.data), view.size);
}

fs::path record_path(const ScopedRoot &root, const ql_digest &key) {
    char hex[QL_DIGEST_HEX_SIZE];
    ql_digest_hex(&key, hex);
    const std::string name(hex);
    return root.path() / ("v" + std::to_string(QL_CACHE_FORMAT_VERSION)) /
           name.substr(0u, 2u) / (name + ".qlc");
}

ql_cache_key_input_v1 make_identity(const char *method,
                                    const char *version,
                                    const char *options) {
    ql_cache_key_input_v1 identity{};
    identity.struct_size = sizeof(identity);
    ql_digest_data("problem artifact", 16u, &identity.artifact_digest);
    ql_digest_data("semantic problem", 16u,
                   &identity.semantic_problem_digest);
    identity.method_name = method;
    identity.method_version = version;
    identity.canonical_options = options;
    identity.canonical_options_size = std::strlen(options);
    return identity;
}

TEST(Cache, StoresAndLoadsAnArtifactAcrossReopening) {
    const ScopedRoot root;
    const ql_digest key = key_of("question one");
    const ArtifactPtr artifact =
        make_artifact(QL_ARTIFACT_KIND_PROOF, "the proof bytes");
    ql_error error{};

    {
        const CachePtr cache = open_cache(root);
        ASSERT_EQ(QL_STATUS_OK,
                  ql_cache_store_artifact(cache.get(), &key, artifact.get(),
                                          &error))
            << error.message;
    }
    {
        /* A fresh handle over the same directory: the point of the store is
           that it outlives the process that filled it. */
        const CachePtr cache = open_cache(root);
        ql_artifact *raw = nullptr;
        ASSERT_EQ(QL_STATUS_OK,
                  ql_cache_load_artifact(cache.get(), &key, &raw, &error))
            << error.message;
        const ArtifactPtr loaded(raw);
        ql_artifact_view view{};
        ASSERT_EQ(QL_STATUS_OK,
                  ql_artifact_get_view(loaded.get(), &view, &error));
        EXPECT_STREQ(QL_ARTIFACT_KIND_PROOF, view.kind);
        EXPECT_EQ("the proof bytes", payload_of(loaded.get()));
    }
}

TEST(Cache, AMissIsNotFoundAndIsCounted) {
    const ScopedRoot root;
    const CachePtr cache = open_cache(root);
    const ql_digest key = key_of("never stored");
    ql_artifact *raw = nullptr;
    ql_error error{};
    std::uint32_t present = 1u;

    EXPECT_EQ(QL_STATUS_OK,
              ql_cache_contains(cache.get(), &key, &present, &error));
    EXPECT_EQ(0u, present);
    EXPECT_EQ(QL_STATUS_NOT_FOUND,
              ql_cache_load_artifact(cache.get(), &key, &raw, &error));
    EXPECT_EQ(nullptr, raw);

    ql_cache_stats_v1 stats{};
    ASSERT_EQ(QL_STATUS_OK,
              ql_cache_get_stats(cache.get(), &stats, &error));
    EXPECT_EQ(1u, stats.misses);
    EXPECT_EQ(0u, stats.hits);
}

TEST(Cache, RestoringIdenticalContentSucceedsAndDivergentContentDoesNot) {
    const ScopedRoot root;
    const CachePtr cache = open_cache(root);
    const ql_digest key = key_of("one question");
    const ArtifactPtr first =
        make_artifact(QL_ARTIFACT_KIND_PROOF, "answer");
    const ArtifactPtr same = make_artifact(QL_ARTIFACT_KIND_PROOF, "answer");
    const ArtifactPtr other =
        make_artifact(QL_ARTIFACT_KIND_PROOF, "different answer");
    ql_error error{};

    ASSERT_EQ(QL_STATUS_OK,
              ql_cache_store_artifact(cache.get(), &key, first.get(),
                                      &error));
    EXPECT_EQ(QL_STATUS_OK,
              ql_cache_store_artifact(cache.get(), &key, same.get(), &error));
    /* Two different answers under one key means the key does not identify the
       question. Overwriting would turn that into a silent wrong answer. */
    EXPECT_EQ(QL_STATUS_ALREADY_EXISTS,
              ql_cache_store_artifact(cache.get(), &key, other.get(),
                                      &error));
    EXPECT_NE('\0', error.message[0]);
}

TEST(Cache, ACorruptedRecordIsRefusedRatherThanReturned) {
    const ScopedRoot root;
    const ql_digest key = key_of("question");
    const ArtifactPtr artifact =
        make_artifact(QL_ARTIFACT_KIND_PROOF, "trustworthy bytes");
    ql_error error{};
    {
        const CachePtr cache = open_cache(root);
        ASSERT_EQ(QL_STATUS_OK,
                  ql_cache_store_artifact(cache.get(), &key, artifact.get(),
                                          &error));
    }

    const fs::path path = record_path(root, key);
    ASSERT_TRUE(fs::exists(path)) << path.string();
    std::vector<char> bytes;
    {
        std::ifstream input(path, std::ios::binary);
        bytes.assign(std::istreambuf_iterator<char>(input),
                     std::istreambuf_iterator<char>());
    }
    ASSERT_FALSE(bytes.empty());
    /* Flip a byte inside the payload, past the header. */
    bytes[bytes.size() - QL_DIGEST_SIZE - 1u] ^= 0x40;
    {
        std::ofstream output(path, std::ios::binary | std::ios::trunc);
        output.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    }

    const CachePtr cache = open_cache(root);
    ql_artifact *raw = nullptr;
    EXPECT_EQ(QL_STATUS_IO_ERROR,
              ql_cache_load_artifact(cache.get(), &key, &raw, &error));
    EXPECT_EQ(nullptr, raw);

    ql_cache_stats_v1 stats{};
    ASSERT_EQ(QL_STATUS_OK,
              ql_cache_get_stats(cache.get(), &stats, &error));
    EXPECT_EQ(1u, stats.rejected_records);
    /* A refusal is not a miss: a store handing out wrong answers must not
       look like an empty one. */
    EXPECT_EQ(0u, stats.misses);
}

TEST(Cache, ATruncatedRecordIsRefused) {
    const ScopedRoot root;
    const ql_digest key = key_of("question");
    ql_error error{};
    {
        const CachePtr cache = open_cache(root);
        const ArtifactPtr artifact =
            make_artifact(QL_ARTIFACT_KIND_PROOF, "some reasonably long body");
        ASSERT_EQ(QL_STATUS_OK,
                  ql_cache_store_artifact(cache.get(), &key, artifact.get(),
                                          &error));
    }
    const fs::path path = record_path(root, key);
    fs::resize_file(path, fs::file_size(path) - 8u);

    const CachePtr cache = open_cache(root);
    ql_artifact *raw = nullptr;
    const ql_status status =
        ql_cache_load_artifact(cache.get(), &key, &raw, &error);
    EXPECT_TRUE(status == QL_STATUS_PARSE_ERROR ||
                status == QL_STATUS_IO_ERROR)
        << ql_status_string(status);
    EXPECT_EQ(nullptr, raw);
}

TEST(Cache, ARecordFiledUnderTheWrongKeyIsRefused) {
    const ScopedRoot root;
    const ql_digest key = key_of("question");
    const ql_digest other = key_of("a different question");
    ql_error error{};
    {
        const CachePtr cache = open_cache(root);
        const ArtifactPtr artifact =
            make_artifact(QL_ARTIFACT_KIND_PROOF, "an answer");
        ASSERT_EQ(QL_STATUS_OK,
                  ql_cache_store_artifact(cache.get(), &key, artifact.get(),
                                          &error));
    }
    const fs::path source = record_path(root, key);
    const fs::path target = record_path(root, other);
    fs::create_directories(target.parent_path());
    fs::copy_file(source, target);

    const CachePtr cache = open_cache(root);
    ql_artifact *raw = nullptr;
    /* The key is written into the record, so a file that ends up in the wrong
       place cannot answer for another question. */
    EXPECT_EQ(QL_STATUS_IO_ERROR,
              ql_cache_load_artifact(cache.get(), &other, &raw, &error));
    EXPECT_EQ(nullptr, raw);
}

TEST(Cache, AnEvidenceEnvelopeRoundTripsAndRecomputesItsKey) {
    const ScopedRoot root;
    constexpr char options[] = "{\"timeout_ms\":5000}";
    ql_cache_key_input_v1 identity =
        make_identity("prove.smt-product", "2.1.0", options);
    ql_digest key{};
    ql_error error{};
    ASSERT_EQ(QL_STATUS_OK, ql_cache_key_compute(&identity, &key, &error));

    const ArtifactPtr evidence =
        make_artifact(QL_ARTIFACT_KIND_COUNTEREXAMPLE, "replayed witness");
    ql_evidence_envelope *raw_envelope = nullptr;
    ASSERT_EQ(QL_STATUS_OK,
              ql_evidence_envelope_create(nullptr, &identity,
                                          QL_EVIDENCE_COUNTEREXAMPLE,
                                          evidence.get(), &raw_envelope,
                                          &error))
        << error.message;
    const EnvelopePtr envelope(raw_envelope);

    {
        const CachePtr cache = open_cache(root);
        ASSERT_EQ(QL_STATUS_OK,
                  ql_cache_store_evidence(cache.get(), envelope.get(),
                                          options, std::strlen(options),
                                          &error))
            << error.message;
    }
    const CachePtr cache = open_cache(root);
    ql_evidence_envelope *loaded_raw = nullptr;
    ASSERT_EQ(QL_STATUS_OK,
              ql_cache_load_evidence(cache.get(), &key, &loaded_raw, &error))
        << error.message;
    const EnvelopePtr loaded(loaded_raw);

    ql_evidence_envelope_view_v1 view{};
    ASSERT_EQ(QL_STATUS_OK,
              ql_evidence_envelope_get_view(loaded.get(), &view, &error));
    EXPECT_EQ(QL_EVIDENCE_COUNTEREXAMPLE, view.evidence_class);
    EXPECT_STREQ("prove.smt-product", view.method_name);
    EXPECT_STREQ("2.1.0", view.method_version);
    /* The envelope recomputed the key from the stored identity rather than
       trusting the one the file was filed under. */
    EXPECT_TRUE(ql_digest_equal(&key, &view.cache_key));
    EXPECT_TRUE(ql_digest_equal(&identity.artifact_digest,
                                &view.artifact_digest));
    ASSERT_NE(nullptr, view.evidence_artifact);
    EXPECT_EQ("replayed witness", payload_of(view.evidence_artifact));
}

TEST(Cache, AnArtifactRecordDoesNotAnswerAnEvidenceLookup) {
    const ScopedRoot root;
    const CachePtr cache = open_cache(root);
    const ql_digest key = key_of("question");
    const ArtifactPtr artifact =
        make_artifact(QL_ARTIFACT_KIND_PROOF, "bytes");
    ql_error error{};
    ASSERT_EQ(QL_STATUS_OK,
              ql_cache_store_artifact(cache.get(), &key, artifact.get(),
                                      &error));
    ql_evidence_envelope *raw = nullptr;
    EXPECT_EQ(QL_STATUS_TYPE_MISMATCH,
              ql_cache_load_evidence(cache.get(), &key, &raw, &error));
    EXPECT_EQ(nullptr, raw);
}

TEST(Cache, RemovingAnEntryMakesItAMissAgain) {
    const ScopedRoot root;
    const CachePtr cache = open_cache(root);
    const ql_digest key = key_of("question");
    const ArtifactPtr artifact =
        make_artifact(QL_ARTIFACT_KIND_PROOF, "bytes");
    ql_error error{};
    std::uint32_t present = 0u;

    ASSERT_EQ(QL_STATUS_OK,
              ql_cache_store_artifact(cache.get(), &key, artifact.get(),
                                      &error));
    ASSERT_EQ(QL_STATUS_OK,
              ql_cache_contains(cache.get(), &key, &present, &error));
    EXPECT_EQ(1u, present);
    EXPECT_EQ(QL_STATUS_OK, ql_cache_remove(cache.get(), &key, &error));
    EXPECT_EQ(QL_STATUS_NOT_FOUND,
              ql_cache_remove(cache.get(), &key, &error));
    ASSERT_EQ(QL_STATUS_OK,
              ql_cache_contains(cache.get(), &key, &present, &error));
    EXPECT_EQ(0u, present);
}

TEST(Cache, AReadOnlyCacheReadsAndRefusesToWrite) {
    const ScopedRoot root;
    const ql_digest key = key_of("question");
    ql_error error{};
    {
        const CachePtr writable = open_cache(root);
        const ArtifactPtr artifact =
            make_artifact(QL_ARTIFACT_KIND_PROOF, "bytes");
        ASSERT_EQ(QL_STATUS_OK,
                  ql_cache_store_artifact(writable.get(), &key,
                                          artifact.get(), &error));
    }
    const CachePtr cache = open_cache(root, true);
    ql_artifact *raw = nullptr;
    ASSERT_EQ(QL_STATUS_OK,
              ql_cache_load_artifact(cache.get(), &key, &raw, &error));
    ql_artifact_release(raw);

    const ArtifactPtr other =
        make_artifact(QL_ARTIFACT_KIND_PROOF, "other bytes");
    const ql_digest other_key = key_of("another question");
    EXPECT_EQ(QL_STATUS_IO_ERROR,
              ql_cache_store_artifact(cache.get(), &other_key, other.get(),
                                      &error));
    EXPECT_EQ(QL_STATUS_IO_ERROR,
              ql_cache_remove(cache.get(), &key, &error));
}

TEST(Cache, HonoursTheEntrySizeLimit) {
    const ScopedRoot root;
    ql_cache_config_v1 config{};
    ql_cache_config_init(&config);
    const std::string text = root.string();
    config.root_path = text.c_str();
    config.max_entry_bytes = 64u;
    ql_cache *raw = nullptr;
    ql_error error{};
    ASSERT_EQ(QL_STATUS_OK, ql_cache_open(nullptr, &config, &raw, &error));
    const CachePtr cache(raw);

    const ql_digest key = key_of("question");
    const ArtifactPtr artifact = make_artifact(
        QL_ARTIFACT_KIND_PROOF,
        "a payload that pushes the encoded record past sixty-four bytes");
    EXPECT_EQ(QL_STATUS_INVALID_ARGUMENT,
              ql_cache_store_artifact(cache.get(), &key, artifact.get(),
                                      &error));
}

TEST(Cache, RejectsAMalformedConfigurationAndNullArguments) {
    const ScopedRoot root;
    const std::string text = root.string();
    ql_cache_config_v1 config{};
    ql_cache *raw = nullptr;
    ql_error error{};

    ql_cache_config_init(&config);
    config.root_path = text.c_str();
    config.abi_version += 1u;
    EXPECT_EQ(QL_STATUS_ABI_MISMATCH,
              ql_cache_open(nullptr, &config, &raw, &error));

    ql_cache_config_init(&config);
    config.root_path = text.c_str();
    config.format_version += 1u;
    EXPECT_EQ(QL_STATUS_SCHEMA_MISMATCH,
              ql_cache_open(nullptr, &config, &raw, &error));

    ql_cache_config_init(&config);
    config.struct_size -= 1u;
    config.root_path = text.c_str();
    EXPECT_EQ(QL_STATUS_ABI_MISMATCH,
              ql_cache_open(nullptr, &config, &raw, &error));

    /* The module never picks a directory of its own. */
    ql_cache_config_init(&config);
    EXPECT_EQ(QL_STATUS_INVALID_ARGUMENT,
              ql_cache_open(nullptr, &config, &raw, &error));

    const CachePtr cache = open_cache(root);
    const ql_digest key = key_of("question");
    EXPECT_EQ(QL_STATUS_INVALID_ARGUMENT,
              ql_cache_store_artifact(cache.get(), &key, nullptr, &error));
    EXPECT_EQ(QL_STATUS_INVALID_ARGUMENT,
              ql_cache_load_artifact(cache.get(), nullptr, nullptr, &error));
    EXPECT_EQ(QL_STATUS_INVALID_ARGUMENT,
              ql_cache_store_evidence(cache.get(), nullptr, nullptr, 0u,
                                      &error));
    ql_cache_close(nullptr);
}

}  // namespace
