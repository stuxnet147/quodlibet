#include "quodlibet/method.h"

#include <stdio.h>
#include <string.h>

#include "uv.h"

static uv_once_t host_once = UV_ONCE_INIT;
static ql_host_v1 default_host;

static void QL_CALL default_log(ql_log_level level, const char *component,
                                const char *message) {
    static const char *levels[] = {
        "trace", "debug", "info", "warning", "error"
    };
    const char *level_name = "unknown";

    if ((unsigned)level < sizeof(levels) / sizeof(levels[0])) {
        level_name = levels[level];
    }
    (void)fprintf(stderr, "[quodlibet:%s:%s] %s\n", level_name,
                  component != NULL ? component : "plugin",
                  message != NULL ? message : "");
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
