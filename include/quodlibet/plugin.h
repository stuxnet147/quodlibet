#ifndef QUODLIBET_PLUGIN_H
#define QUODLIBET_PLUGIN_H

#include "quodlibet/registry.h"

#define QL_PLUGIN_ENTRY_SYMBOL "quodlibet_plugin_init_v1"

QL_EXTERN_C_BEGIN

typedef struct ql_plugin_v1 {
    size_t struct_size;
    uint32_t abi_version;
    const char *name;
    const char *version;
    const ql_method_v1 *methods;
    size_t method_count;
    void (QL_CALL *shutdown)(void);
    void *reserved[8];
    /* Optional append-only extension. Descriptors decorate entries in methods
       and are discovered automatically while the plugin is loaded. */
    const ql_proof_method_v1 *proof_methods;
    size_t proof_method_count;
} ql_plugin_v1;

typedef ql_status (QL_CALL *ql_plugin_init_v1_fn)(
    const ql_host_v1 *host, const ql_plugin_v1 **plugin, ql_error *error);

typedef struct ql_plugin_handle ql_plugin_handle;

QL_API ql_status QL_CALL ql_plugin_load(ql_registry *registry,
                                        const char *path,
                                        ql_plugin_handle **output,
                                        ql_error *error);
QL_API const char *QL_CALL ql_plugin_name(const ql_plugin_handle *plugin);
QL_API const char *QL_CALL ql_plugin_version(const ql_plugin_handle *plugin);
QL_API void QL_CALL ql_plugin_unload(ql_plugin_handle *plugin);

QL_EXTERN_C_END

#endif
