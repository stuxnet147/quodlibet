#include "quodlibet/method.h"

#include <string.h>

#include "uv.h"

static uv_once_t host_once = UV_ONCE_INIT;
static ql_host_v1 default_host;

/* Plugin diagnostics go through the public logging service instead of the
   caller's stderr, so a host that installed no sink stays silent. */
static void QL_CALL default_log(ql_log_level level, const char *component,
                                const char *message) {
    ql_log_write(level, component != NULL ? component : "plugin", NULL, NULL,
                 0u, "%s", message != NULL ? message : "");
}

static void initialize_host(void) {
    memset(&default_host, 0, sizeof(default_host));
    default_host.struct_size = sizeof(default_host);
    default_host.abi_version = QL_ABI_VERSION;
    default_host.allocator = *ql_default_allocator();
    default_host.artifact_create = ql_artifact_create;
    default_host.artifact_retain = ql_artifact_retain;
    default_host.artifact_release = ql_artifact_release;
    default_host.log = default_log;
}

const ql_host_v1 *QL_CALL ql_default_host(void) {
    uv_once(&host_once, initialize_host);
    return &default_host;
}
