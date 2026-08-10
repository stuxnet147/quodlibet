# A minimal Quodlibet plugin

`normalize_plugin.c` is the smallest plugin that is still a real one: it
registers one method, allocates through the host, produces a new artifact, and
declares nothing it cannot back up.

It is built by the repository's own CMake and loaded by a test
(`tests/test_example_plugin.cpp`), so it cannot rot into an example that no
longer compiles. An example that has stopped working is not an example, it is
a trap.

## What a plugin must get right

Four things, and the example is arranged so each one is visible.

**The entry point is the whole ABI handshake.** The host calls
`quodlibet_plugin_init_v1` and passes its own `ql_host_v1`. Check
`host->abi_version` and return `QL_STATUS_ABI_MISMATCH` if it is not yours.
That check is what makes it safe to load a plugin built against a different
release.

**Every structure states its own size and generation.** `struct_size` and
`abi_version` come first in `ql_plugin_v1` and `ql_method_v1` and the host
reads them before anything else. Fill them with `sizeof` and `QL_ABI_VERSION`
from the headers you compiled against. Structures grow append-only, so a
plugin built today keeps loading into a later host: the host reads the prefix
it understands. `tests/test_abi_compat.cpp` holds that promise in place.

**Memory crosses the boundary only through the host.** The plugin and the host
may be linked against different C runtimes, and a block allocated by one
runtime cannot be freed by the other. So allocate artifacts with
`context->host->artifact_create`, never with `malloc` plus a hand-rolled
struct. Ownership then follows one rule: an artifact handed to `run` as an
input is borrowed, and the artifact `run` writes to `*output` is transferred to
the caller.

**Say only what you can support.** `minimum_inputs` and `maximum_inputs` are
checked by the host before `run` is called. Flags such as
`QL_METHOD_PROOF_PRODUCER` are claims about soundness, not hints; a method that
sets one and returns an ordinary artifact is lying to the pipeline. The example
sets only `QL_METHOD_DETERMINISTIC` and `QL_METHOD_CACHEABLE`, both of which it
actually satisfies.

## Building it

It builds with the repository:

```sh
cmake --preset windows-clang
cmake --build --preset windows-clang --parallel
```

The module lands next to the other build outputs as
`ql_example_plugin.dll` on Windows and `ql_example_plugin.so` elsewhere.

Out of tree, against an installed Quodlibet, the whole build is:

```cmake
find_package(quodlibet REQUIRED)
add_library(my_plugin MODULE my_plugin.c)
target_link_libraries(my_plugin PRIVATE quodlibet::quodlibet)
set_target_properties(my_plugin PROPERTIES PREFIX "" C_VISIBILITY_PRESET hidden)
```

Hidden visibility matters: `QL_PLUGIN_EXPORT` marks the one symbol the host
looks up, and hiding the rest keeps a plugin from accidentally exporting names
that collide with the host's.

## Loading it

```c
ql_registry *registry = NULL;
ql_plugin_handle *plugin = NULL;
ql_error error;

ql_registry_create(NULL, &registry, &error);
ql_plugin_load(registry, "path/to/ql_example_plugin.dll", &plugin, &error);
/* Its methods are now in the registry under the names it declared. */
ql_plugin_unload(plugin);   /* unregisters them again */
ql_registry_destroy(registry);
```

`ql_plugin_unload` removes the plugin's methods from the registry before it
closes the library, so a pipeline built against a since-unloaded method fails
with `QL_STATUS_NOT_FOUND` rather than jumping into unmapped code.
