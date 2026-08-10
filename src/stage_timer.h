#ifndef QUODLIBET_SRC_STAGE_TIMER_H
#define QUODLIBET_SRC_STAGE_TIMER_H

/* Per-judgement stage timing, compiled only when QL_STAGE_TIMING is defined.
   In a normal build every macro below expands to nothing, so the canonical
   path keeps the code it had: no clock call, no branch, no storage.

   This exists because docs/perf/baseline.md could say how long a judgement
   takes but not where the time went inside one. VTune cannot answer it on this
   host: the collector hangs on any workload that forks a child, and every
   judgement forks Bitwuzla. A monotonic clock at the stage boundaries answers
   it without a profiler and without a child-process restriction.

   Only the boundaries are timed, never anything inside a loop, so the number
   of clock calls per judgement is a fixed handful. The measured cost of the
   instrumentation itself is recorded in docs/perf/stages.md.

   Buckets are raw and some of them nest: QL_STAGE_FRONTEND contains
   QL_STAGE_PARSE_FRONTEND, and QL_STAGE_LOWER contains QL_STAGE_PARSE_LOWER.
   The reader subtracts. Storing the inclusive value is what keeps the hook at
   the call site instead of inside the parser.

   Exactly one translation unit defines QL_STAGE_TIMER_DEFINE before including
   this header; every other includer gets the extern declaration. That is what
   lets this be a header with no CMakeLists entry of its own. */

#include <stdint.h>

#if defined(QL_STAGE_TIMING)

#include <stdio.h>
#include <stdlib.h>

#if defined(_WIN32)
#include <windows.h>
#else
#include <time.h>
#include <unistd.h>
#endif

enum {
    QL_STAGE_PROBLEM = 0,
    QL_STAGE_FRONTEND,
    QL_STAGE_PARSE_FRONTEND,
    QL_STAGE_LOWER,
    QL_STAGE_PARSE_LOWER,
    QL_STAGE_IR_OPEN,
    QL_STAGE_PRODUCT,
    QL_STAGE_SMT2,
    QL_STAGE_SOLVER_SETUP,
    QL_STAGE_SOLVER_CHECK,
    /* Inside QL_STAGE_SOLVER_CHECK, so the reader subtracts these out of it
       the same way it subtracts the parses out of the frontend and lowering.
       The three answer different questions about the same round trip: how
       much of it is our own integrity hashing, how much is the kernel putting
       a process on a core, and how much is Bitwuzla actually solving. */
    QL_STAGE_SOLVER_DIGEST,
    QL_STAGE_SOLVER_SPAWN,
    QL_STAGE_SOLVER_RUN,
    QL_STAGE_REPLAY,
    QL_STAGE_OUTCOME,
    QL_STAGE_TOTAL,
    QL_STAGE_SLOT_COUNT
};

typedef struct ql_stage_slots {
    uint64_t ns[QL_STAGE_SLOT_COUNT];
    uint32_t hits[QL_STAGE_SLOT_COUNT];
} ql_stage_slots;

#if defined(QL_STAGE_TIMER_DEFINE)
_Thread_local ql_stage_slots ql_stage_current;
#else
extern _Thread_local ql_stage_slots ql_stage_current;
#endif

static inline uint64_t ql_stage_now_ns(void) {
#if defined(_WIN32)
    static LARGE_INTEGER frequency;
    LARGE_INTEGER counter;
    if (frequency.QuadPart == 0) {
        QueryPerformanceFrequency(&frequency);
    }
    QueryPerformanceCounter(&counter);
    return (uint64_t)((counter.QuadPart * INT64_C(1000000000)) /
                      frequency.QuadPart);
#else
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return (uint64_t)now.tv_sec * UINT64_C(1000000000) +
           (uint64_t)now.tv_nsec;
#endif
}

static inline void ql_stage_add(int slot, uint64_t started_ns) {
    ql_stage_current.ns[slot] += ql_stage_now_ns() - started_ns;
    ql_stage_current.hits[slot] += 1u;
}

static inline void ql_stage_reset(void) {
    int slot;
    for (slot = 0; slot < QL_STAGE_SLOT_COUNT; ++slot) {
        ql_stage_current.ns[slot] = 0u;
        ql_stage_current.hits[slot] = 0u;
    }
}

/* One line per judgement, appended to the file named by QL_STAGE_TIMING_FILE.
   Nothing is written when the variable is unset, so an instrumented binary
   that nobody is measuring behaves like the normal one apart from the clock
   calls. The whole record is formatted into one buffer and written with a
   single call so that concurrent workers interleave lines, not fields. */
static inline void ql_stage_emit(void) {
    const char *path = getenv("QL_STAGE_TIMING_FILE");
    char line[512];
    int written;
    int slot;
    int offset = 0;
    FILE *out;

    if (path == NULL || path[0] == '\0') {
        ql_stage_reset();
        return;
    }
#if defined(_WIN32)
    written = snprintf(line, sizeof(line), "%lu",
                       (unsigned long)GetCurrentThreadId());
#else
    written = snprintf(line, sizeof(line), "%ld", (long)getpid());
#endif
    if (written < 0) {
        ql_stage_reset();
        return;
    }
    offset = written;
    for (slot = 0; slot < QL_STAGE_SLOT_COUNT; ++slot) {
        written = snprintf(line + offset, sizeof(line) - (size_t)offset,
                           "\t%llu",
                           (unsigned long long)ql_stage_current.ns[slot]);
        if (written < 0 || (size_t)(offset + written) >= sizeof(line)) {
            ql_stage_reset();
            return;
        }
        offset += written;
    }
    if ((size_t)offset + 2u >= sizeof(line)) {
        ql_stage_reset();
        return;
    }
    line[offset++] = '\n';
    line[offset] = '\0';
    out = fopen(path, "a");
    if (out != NULL) {
        fwrite(line, 1u, (size_t)offset, out);
        fclose(out);
    }
    ql_stage_reset();
}

/* One line per check-sat, appended to QL_STAGE_QUERY_LOG, and optionally the
   query text itself under QL_STAGE_QUERY_DIR named by its digest.

   The per-judgement record above cannot answer what makes a query slow,
   because a judgement runs two of them and reports their sum. This does:
   size, wall time and answer per query, with the text kept so a slow one can
   be replayed against the backend directly. Nothing is written when the
   variables are unset. */
static inline void ql_stage_query_dump(const char *digest_hex,
                                       const void *text, size_t bytes) {
    const char *dir = getenv("QL_STAGE_QUERY_DIR");
    char path[512];
    int written;
    FILE *out;

    if (dir == NULL || dir[0] == '\0' || text == NULL) {
        return;
    }
    written = snprintf(path, sizeof(path), "%s/%s.smt2", dir, digest_hex);
    if (written <= 0 || (size_t)written >= sizeof(path)) {
        return;
    }
    out = fopen(path, "wb");
    if (out != NULL) {
        fwrite(text, 1u, bytes, out);
        fclose(out);
    }
}

static inline void ql_stage_query_log(const char *digest_hex, size_t bytes,
                                      uint64_t elapsed_ns,
                                      const char *answer) {
    const char *log = getenv("QL_STAGE_QUERY_LOG");
    char line[512];
    int written;
    FILE *out;

    if (log == NULL || log[0] == '\0') {
        return;
    }
    written = snprintf(line, sizeof(line), "%s\t%zu\t%llu\t%s\n",
                       digest_hex, bytes, (unsigned long long)elapsed_ns,
                       answer != NULL ? answer : "?");
    if (written <= 0 || (size_t)written >= sizeof(line)) {
        return;
    }
    out = fopen(log, "a");
    if (out != NULL) {
        fwrite(line, 1u, (size_t)written, out);
        fclose(out);
    }
}

#define QL_STAGE_MARK(name_) const uint64_t name_ = ql_stage_now_ns()
#define QL_STAGE_ADD(slot_, name_) ql_stage_add((slot_), (name_))
#define QL_STAGE_RESET() ql_stage_reset()
#define QL_STAGE_EMIT() ql_stage_emit()
#define QL_STAGE_QUERY_DUMP(digest_, text_, bytes_)                           \
    ql_stage_query_dump((digest_), (text_), (bytes_))
#define QL_STAGE_QUERY_LOG(digest_, bytes_, ns_, answer_)                     \
    ql_stage_query_log((digest_), (bytes_), (ns_), (answer_))
#define QL_STAGE_ELAPSED(name_) (ql_stage_now_ns() - (name_))

#else /* !QL_STAGE_TIMING */

#define QL_STAGE_MARK(name_) ((void)0)
#define QL_STAGE_ADD(slot_, name_) ((void)0)
#define QL_STAGE_RESET() ((void)0)
#define QL_STAGE_EMIT() ((void)0)
#define QL_STAGE_QUERY_DUMP(digest_, text_, bytes_) ((void)0)
#define QL_STAGE_QUERY_LOG(digest_, bytes_, ns_, answer_) ((void)0)
#define QL_STAGE_ELAPSED(name_) UINT64_C(0)

#endif /* QL_STAGE_TIMING */

#endif /* QUODLIBET_SRC_STAGE_TIMER_H */
