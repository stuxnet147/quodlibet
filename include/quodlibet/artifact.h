#ifndef QUODLIBET_ARTIFACT_H
#define QUODLIBET_ARTIFACT_H

#include "quodlibet/allocator.h"
#include "quodlibet/hash.h"
#include "quodlibet/status.h"

QL_EXTERN_C_BEGIN

#define QL_ARTIFACT_KIND_PROBLEM "quodlibet.problem"
#define QL_ARTIFACT_KIND_IR "quodlibet.ir"
#define QL_ARTIFACT_KIND_OUTCOME "quodlibet.outcome"
#define QL_ARTIFACT_KIND_PROOF "quodlibet.proof"
#define QL_ARTIFACT_KIND_COUNTEREXAMPLE "quodlibet.counterexample"

typedef struct ql_artifact ql_artifact;

typedef struct ql_artifact_view {
    size_t struct_size;
    const char *kind;
    uint32_t schema_version;
    const void *data;
    size_t size;
    ql_digest digest;
} ql_artifact_view;

QL_API ql_status QL_CALL ql_artifact_create(
    const ql_allocator *allocator, const char *kind, uint32_t schema_version,
    const void *data, size_t size, ql_artifact **output, ql_error *error);
QL_API void QL_CALL ql_artifact_retain(ql_artifact *artifact);
QL_API void QL_CALL ql_artifact_release(ql_artifact *artifact);
QL_API ql_status QL_CALL ql_artifact_get_view(const ql_artifact *artifact,
                                              ql_artifact_view *view,
                                              ql_error *error);

QL_EXTERN_C_END

#endif
