#ifndef QUODLIBET_SRC_PROCESS_RUNNER_H
#define QUODLIBET_SRC_PROCESS_RUNNER_H

/* One process discipline, two consumers.

   Quodlibet runs external decision procedures as isolated processes: the
   Bitwuzla adapter in src/solver.c, and the CaDiCaL backend and LRAT checker
   behind prove.aig-sat. Both need the same things, and none of them are
   incidental:

     - an argument vector and an absolute path, never a shell;
     - a private snapshot of the executable, so a binary swapped underneath a
       run cannot change what answered;
     - a content digest of the bytes that actually ran, for the evidence and
       the cache key;
     - a watchdog deadline, an escalating kill, and a bounded drain of the
       output pipes after the child exits;
     - hard caps on captured output, reported rather than silently truncated;
     - a cancellation hook checked while the loop runs.

   That discipline is where the repository's hard-won process fixes live: the
   loop-lifetime completion, the shared-read open, the anti-virus retry, and
   the CLOEXEC inheritance block. A second runner would start without any of
   them. This header exists so there is exactly one.

   It is internal. Nothing here crosses the public ABI, and neither consumer
   exposes a process to its callers. The header is written before the
   extraction so that the interface is agreed on its merits rather than
   inherited from whichever call site happened to be moved first.

   Behaviour is preserved by the extraction: the existing solver tests must
   pass unchanged. The one deliberate difference is that the runner takes a
   neutral limits structure instead of ql_solver_check_request_v1, because a
   runner that knows what an SMT check request is cannot serve a checker that
   takes a DIMACS path. */

#include "quodlibet/allocator.h"
#include "quodlibet/hash.h"
#include "quodlibet/status.h"

QL_EXTERN_C_BEGIN

/* Captured output above this is a reported truncation, never a quiet one. A
   solver that floods its output is a solver whose answer is not usable. */
#define QL_PROCESS_DEFAULT_OUTPUT_LIMIT ((size_t)(16u * 1024u * 1024u))

/* The watchdog fires this much after the requested timeout, so a tool given
   its own time limit is allowed to reach that limit and report rather than be
   killed on the boundary. A caller that wants a total wall-clock budget must
   subtract this from what it asks for, which is why the number is here and not
   private to the runner. */
#define QL_PROCESS_WATCHDOG_GRACE_MS UINT64_C(50)

typedef struct ql_process_limits_v1 {
    size_t struct_size;
    /* Zero disables the watchdog. A run that hits it is reported as timed
       out, which a caller must turn into UNKNOWN and never into a verdict. */
    uint64_t timeout_ms;
    /* Advisory, applied where the platform can. Zero means unlimited. */
    uint64_t memory_limit_mb;
    /* Zero selects QL_PROCESS_DEFAULT_OUTPUT_LIMIT. */
    size_t stdout_limit_bytes;
    size_t stderr_limit_bytes;
    /* Polled while the loop runs. Both may be null. */
    const void *cancel_state;
    uint32_t (QL_CALL *is_cancelled)(const void *cancel_state);
    uint64_t reserved[4];
} ql_process_limits_v1;

typedef struct ql_process_result_v1 {
    size_t struct_size;
    /* Owned by the result until ql_process_result_dispose. Always
       null-terminated, so a caller may scan them as text. */
    char *stdout_text;
    size_t stdout_size;
    char *stderr_text;
    size_t stderr_size;
    int64_t exit_status;
    int32_t term_signal;
    /* Exactly the observations that separate "the tool answered" from "the
       tool was stopped". A caller that ignores these turns a killed process
       into a logical conclusion. */
    uint32_t cancelled;
    uint32_t timed_out;
    uint32_t killed;
    uint32_t stdout_truncated;
    uint32_t stderr_truncated;
    uint32_t reserved32;
    uint64_t reserved[4];
} ql_process_result_v1;

/* A private copy of an executable, plus the digest of the bytes copied. The
   digest is of the snapshot, not of the source path, because the snapshot is
   what runs. */
typedef struct ql_process_snapshot {
    ql_allocator allocator;
    char *directory;
    char *executable_path;
    ql_digest digest;
} ql_process_snapshot;

void ql_process_limits_init(ql_process_limits_v1 *limits);
void ql_process_result_init(ql_process_result_v1 *result);
void ql_process_result_dispose(const ql_allocator *allocator,
                               ql_process_result_v1 *result);

/* Copies `source_executable` into a fresh private directory, marks it
   executable, and records its content digest. `name` is the base name the copy
   gets, so a diagnostic names the tool rather than a temporary. */
ql_status ql_process_snapshot_create(const ql_allocator *allocator,
                                     const char *source_executable,
                                     const char *name,
                                     ql_process_snapshot *snapshot,
                                     ql_error *error);
/* Removes the copy and its directory. Safe on a zeroed or failed snapshot. */
void ql_process_snapshot_dispose(ql_process_snapshot *snapshot);

/* The digest of an executable's bytes without copying it, for a caller that
   runs a path it does not own. */
ql_status ql_process_executable_digest(const char *path, ql_digest *digest,
                                       ql_error *error);

/* A private directory for the files a run needs on disk.

   Not every tool reads a pipe. CaDiCaL takes a DIMACS path and writes its LRAT
   proof to a second path, and the checker then reads both. Those files must
   live somewhere no other run can see, must be opened with the same
   close-on-exec discipline as everything else here, and must not outlive the
   run that made them. That is the same problem the snapshot solves, so it is
   solved in the same place rather than a second time in a method. */
typedef struct ql_process_scratch {
    ql_allocator allocator;
    char *directory;
} ql_process_scratch;

/* Longest path this module will build for a scratch file. */
#define QL_PROCESS_SCRATCH_PATH_CAPACITY 32768u

ql_status ql_process_scratch_create(const ql_allocator *allocator,
                                    const char *name,
                                    ql_process_scratch *scratch,
                                    ql_error *error);
/* Removes every file directly inside the directory and then the directory.
   Safe on a zeroed or failed scratch. */
void ql_process_scratch_dispose(ql_process_scratch *scratch);

/* The absolute path a file with this base name would have. `name` may not
   contain a path separator, so a caller cannot address anything outside. */
ql_status ql_process_scratch_path(const ql_process_scratch *scratch,
                                  const char *name, char *path,
                                  size_t capacity, ql_error *error);
/* Writes `bytes` to that path, replacing anything there. */
ql_status ql_process_scratch_write(const ql_process_scratch *scratch,
                                   const char *name, const void *bytes,
                                   size_t size, ql_error *error);
/* Reads it back, terminated and with the terminator uncounted, up to
   `limit` bytes. A file above the limit is a reported failure, never a
   silent prefix. The caller frees `text` with the same allocator. */
ql_status ql_process_scratch_read(const ql_process_scratch *scratch,
                                  const char *name, size_t limit,
                                  char **text, size_t *size,
                                  ql_error *error);

/* Non-zero when `path` can be opened for reading under the same close-on-exec
   and shared-read discipline every other open in this module uses. A caller
   probing candidate executables must not open them any other way: a descriptor
   that survives a fork is what makes a sibling judgement die with ETXTBSY, and
   an exclusive open is what makes an antivirus scan look like a missing file.
   That is why this is here rather than reimplemented at the call site. */
int ql_process_path_is_readable(const char *path);

/* Runs `executable_path` with `arguments`, which must not include argv[0]:
   the runner supplies the path as argv[0] itself so a caller cannot
   accidentally disagree with what it spawned. `input` may be null.

   A non-OK status means the run could not be carried out. It is never a
   statement about the question the process was asked; that lives entirely in
   the result. */
ql_status ql_process_run(const ql_allocator *allocator,
                         const char *executable_path,
                         const char *const *arguments, size_t argument_count,
                         const void *input, size_t input_size,
                         const ql_process_limits_v1 *limits,
                         ql_process_result_v1 *result, ql_error *error);

QL_EXTERN_C_END

#endif
