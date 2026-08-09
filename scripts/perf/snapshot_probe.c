/* What does one judgement pay for the private Bitwuzla snapshot?
 *
 *   snapshot_probe <source-executable> <iterations>
 *
 * Every ql_py_check builds a fresh registry, so the Bitwuzla backend state is
 * created and destroyed once per judgement (bindings/python/src/ql_check.c:506).
 * That state copies the selected executable into a private temporary directory
 * and then hashes it: once at creation, once during the version probe, and
 * twice per solver check, before and during (src/solver.c:2341..2761).
 *
 * This replays exactly that file work with the same primitives and nothing
 * else, so the number it prints can be set against the measured 87 ms per
 * judgement. It deliberately does NOT solve anything. It answers one
 * question: how much of a judgement is the snapshot, before anyone proposes
 * changing a design whose whole point is a per-instance identity that cannot
 * be swapped underneath a running check.
 *
 * Built ad hoc by scripts/perf/bench-snapshot.sh. It is a measurement probe,
 * not part of the library, so it is not in CMakeLists.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/stat.h>

#include "blake3.h"

#define CHUNK 65536u
#define HASHES_PER_JUDGEMENT 4u

static double now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000.0 + ts.tv_nsec / 1000000.0;
}

/* src/solver.c:1882 copy_snapshot_bytes, same stdio primitives. */
static long copy_file(const char *from, const char *to) {
    unsigned char bytes[CHUNK];
    FILE *in = fopen(from, "rb");
    FILE *out;
    size_t count;
    long total = 0;

    if (in == NULL) {
        return -1;
    }
    out = fopen(to, "wb");
    if (out == NULL) {
        fclose(in);
        return -1;
    }
    while ((count = fread(bytes, 1u, sizeof(bytes), in)) != 0u) {
        if (fwrite(bytes, 1u, count, out) != count) {
            fclose(in);
            fclose(out);
            return -1;
        }
        total += (long)count;
    }
    fclose(in);
    if (fclose(out) != 0) {
        return -1;
    }
    return total;
}

/* src/solver.c:2216 digest_executable, same chunk size and hasher. */
static int digest_file(const char *path, unsigned char out[32]) {
    unsigned char bytes[CHUNK];
    blake3_hasher hasher;
    FILE *file = fopen(path, "rb");
    size_t count;

    if (file == NULL) {
        return -1;
    }
    blake3_hasher_init(&hasher);
    while ((count = fread(bytes, 1u, sizeof(bytes), file)) != 0u) {
        blake3_hasher_update(&hasher, bytes, count);
    }
    fclose(file);
    blake3_hasher_finalize(&hasher, out, 32u);
    return 0;
}

int main(int argc, char **argv) {
    const char *source;
    long iterations;
    long index;
    double copy_ms = 0.0;
    double hash_ms = 0.0;
    long size = 0;
    char directory[] = "/tmp/quodlibet-snapshot-probe-XXXXXX";
    char snapshot[256];
    unsigned char digest[32] = { 0 };

    if (argc != 3) {
        fprintf(stderr, "usage: snapshot_probe <source-executable> <iterations>\n");
        return 2;
    }
    source = argv[1];
    iterations = strtol(argv[2], NULL, 10);
    if (iterations <= 0) {
        fprintf(stderr, "iterations must be positive\n");
        return 2;
    }
    if (mkdtemp(directory) == NULL) {
        perror("mkdtemp");
        return 1;
    }
    snprintf(snapshot, sizeof(snapshot), "%s/bitwuzla", directory);

    for (index = 0; index < iterations; ++index) {
        double start = now_ms();
        unsigned hash;

        size = copy_file(source, snapshot);
        if (size < 0) {
            fprintf(stderr, "copy failed\n");
            return 1;
        }
        copy_ms += now_ms() - start;

        start = now_ms();
        for (hash = 0u; hash < HASHES_PER_JUDGEMENT; ++hash) {
            if (digest_file(snapshot, digest) != 0) {
                fprintf(stderr, "digest failed\n");
                return 1;
            }
        }
        hash_ms += now_ms() - start;
        unlink(snapshot);
    }
    rmdir(directory);

    printf("bytes         %ld\n", size);
    printf("iterations    %ld\n", iterations);
    printf("copy          %.3f ms each\n", copy_ms / (double)iterations);
    printf("hash x%u      %.3f ms each\n", HASHES_PER_JUDGEMENT,
           hash_ms / (double)iterations);
    printf("total         %.3f ms per judgement\n",
           (copy_ms + hash_ms) / (double)iterations);
    /* Printed so a faster BLAKE3 build can be shown to produce the same
       identity rather than assumed to. A digest that changed would make the
       speedup worthless: the snapshot's whole purpose is that identity. */
    printf("digest        ");
    for (index = 0; index < 32; ++index) {
        printf("%02x", digest[index]);
    }
    printf("\n");
    return 0;
}
