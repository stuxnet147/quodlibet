/* The example plugin, exercised as a host would exercise it.

   examples/plugin/ exists to show someone how to write a plugin. An example
   that has quietly stopped compiling, or that still compiles but no longer
   does what its README claims, is worse than no example: it is a trap that
   costs the reader a debugging session to discover. So the example is built
   by the repository and loaded here.

   What is checked is what the README promises, in the same order it promises
   it: the entry point refuses a foreign ABI, the descriptor declares its own
   size and generation, the method's declared arity is enforced, ownership
   across the boundary is what it says, and unloading takes the method back
   out of the registry. */

#include "quodlibet/plugin.h"
#include "quodlibet/registry.h"

#include <cstddef>
#include <cstring>
#include <filesystem>
#include <string>

#if defined(_WIN32)
#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <dlfcn.h>
#endif

#include <gtest/gtest.h>

namespace {

namespace fs = std::filesystem;

fs::path this_executable() {
#if defined(_WIN32)
    std::wstring path(32768u, L'\0');
    const DWORD size =
        GetModuleFileNameW(nullptr, path.data(),
                           static_cast<DWORD>(path.size()));
    if (size == 0u || size >= path.size()) {
        return {};
    }
    path.resize(size);
    return fs::path(path);
#else
    std::error_code error;
    return fs::read_symlink("/proc/self/exe", error);
#endif
}

/* examples/CMakeLists.txt emits the module beside this executable, so the
   path follows from argv[0] rather than from a build-system variable this
   file would have to be handed. */
fs::path example_plugin_path() {
    const fs::path host = this_executable();
    if (host.empty()) {
        return {};
    }
#if defined(_WIN32)
    return host.parent_path() / "ql_example_plugin.dll";
#elif defined(__APPLE__)
    return host.parent_path() / "ql_example_plugin.dylib";
#else
    return host.parent_path() / "ql_example_plugin.so";
#endif
}

/* The module opened with the platform loader rather than through
   ql_plugin_load, so the entry point can be called with hosts the host itself
   would never present. */
class RawModule {
public:
    explicit RawModule(const std::string &path) {
#if defined(_WIN32)
        handle_ = LoadLibraryA(path.c_str());
#else
        handle_ = dlopen(path.c_str(), RTLD_NOW | RTLD_LOCAL);
#endif
    }
    RawModule(const RawModule &) = delete;
    RawModule &operator=(const RawModule &) = delete;
    ~RawModule() {
        if (handle_ == nullptr) {
            return;
        }
#if defined(_WIN32)
        FreeLibrary(handle_);
#else
        dlclose(handle_);
#endif
    }

    bool opened() const { return handle_ != nullptr; }

    ql_plugin_init_v1_fn entry() const {
        if (handle_ == nullptr) {
            return nullptr;
        }
#if defined(_WIN32)
        FARPROC symbol = GetProcAddress(handle_, QL_PLUGIN_ENTRY_SYMBOL);
#else
        void *symbol = dlsym(handle_, QL_PLUGIN_ENTRY_SYMBOL);
#endif
        ql_plugin_init_v1_fn function = nullptr;
        if (symbol == nullptr) {
            return nullptr;
        }
        /* A function pointer cannot be cast from an object pointer portably,
           so it is copied rather than reinterpreted. */
        std::memcpy(&function, &symbol, sizeof(function));
        return function;
    }

private:
#if defined(_WIN32)
    HMODULE handle_ = nullptr;
#else
    void *handle_ = nullptr;
#endif
};

struct RegistryHandle {
    ql_registry *value = nullptr;

    RegistryHandle() {
        ql_error error{};
        EXPECT_EQ(QL_STATUS_OK, ql_registry_create(nullptr, &value, &error))
            << error.message;
    }
    ~RegistryHandle() { ql_registry_destroy(value); }
};

class ExamplePlugin : public ::testing::Test {
protected:
    void SetUp() override {
        path_ = example_plugin_path();
        /* A missing module is stated, never skipped. The example is part of
           this build; if it is absent, something did not build and a green
           run would say the example still works. */
        ASSERT_FALSE(path_.empty()) << "could not locate the test executable";
        ASSERT_TRUE(fs::exists(path_))
            << "the example plugin was not built at " << path_.string()
            << "; examples/CMakeLists.txt should have produced it";
    }

    std::string path() const { return path_.string(); }

private:
    fs::path path_;
};

/* Loading it registers exactly what it declared, under the name the README
   tells a reader to expect. */
TEST_F(ExamplePlugin, LoadsAndRegistersTheMethodItDeclares) {
    RegistryHandle registry;
    ql_plugin_handle *plugin = nullptr;
    ql_error error{};

    ASSERT_EQ(QL_STATUS_OK,
              ql_plugin_load(registry.value, path().c_str(), &plugin, &error))
        << error.message;
    EXPECT_STREQ("quodlibet-example-plugin", ql_plugin_name(plugin));
    EXPECT_STREQ("1.0.0", ql_plugin_version(plugin));

    const ql_method_v1 *method =
        ql_registry_find(registry.value, "example.normalize");
    ASSERT_NE(nullptr, method);
    /* The descriptor states its own size and generation, which is what lets a
       later host read the prefix it understands. */
    EXPECT_EQ(QL_ABI_VERSION, method->abi_version);
    EXPECT_GE(method->struct_size, offsetof(ql_method_v1, reserved));
    /* It claims determinism and cacheability, which a pure byte rewrite has,
       and claims nothing about proofs or counterexamples, which it does not
       produce. A method that claimed either would be lying to the pipeline. */
    EXPECT_NE(0u, method->flags & QL_METHOD_DETERMINISTIC);
    EXPECT_NE(0u, method->flags & QL_METHOD_CACHEABLE);
    EXPECT_EQ(0u, method->flags & QL_METHOD_PROOF_PRODUCER);
    EXPECT_EQ(0u, method->flags & QL_METHOD_COUNTEREXAMPLE_PRODUCER);
    EXPECT_EQ(1u, method->minimum_inputs);
    EXPECT_EQ(1u, method->maximum_inputs);
    /* No proof-method descriptors, so the registry gains none. */
    EXPECT_EQ(nullptr,
              ql_registry_find_proof_method(registry.value,
                                            "example.normalize"));

    ql_plugin_unload(plugin);
}

/* The method does what the README says it does, including across the
   ownership boundary: the input is borrowed and survives, the output is a new
   artifact the caller owns. */
TEST_F(ExamplePlugin, NormalizesWhitespaceAndTransfersTheOutput) {
    RegistryHandle registry;
    ql_plugin_handle *plugin = nullptr;
    ql_artifact *input = nullptr;
    ql_artifact *output = nullptr;
    ql_artifact_view view{};
    ql_run_context_v1 context{};
    void *instance = nullptr;
    ql_error error{};
    const char source[] = "  a\t\tb \n c  ";

    ASSERT_EQ(QL_STATUS_OK,
              ql_plugin_load(registry.value, path().c_str(), &plugin, &error))
        << error.message;
    const ql_method_v1 *method =
        ql_registry_find(registry.value, "example.normalize");
    ASSERT_NE(nullptr, method);

    ASSERT_EQ(QL_STATUS_OK,
              ql_artifact_create(nullptr, QL_ARTIFACT_KIND_PROBLEM, 1u, source,
                                 sizeof(source) - 1u, &input, &error))
        << error.message;
    ASSERT_NE(nullptr, method->create);
    ASSERT_EQ(QL_STATUS_OK,
              method->create(ql_default_host(), nullptr, &instance, &error))
        << error.message;
    ASSERT_EQ(QL_STATUS_OK, method->validate(instance, &input, 1u, &error))
        << error.message;

    context.struct_size = sizeof(context);
    context.abi_version = QL_ABI_VERSION;
    context.host = ql_default_host();
    ASSERT_EQ(QL_STATUS_OK,
              method->run(instance, &context, &input, 1u, &output, &error))
        << error.message;

    ASSERT_NE(nullptr, output);
    view.struct_size = sizeof(view);
    ASSERT_EQ(QL_STATUS_OK, ql_artifact_get_view(output, &view, &error));
    EXPECT_STREQ("example.normalized", view.kind);
    EXPECT_EQ(std::string("a b c"),
              std::string(static_cast<const char *>(view.data), view.size));

    /* The input was borrowed, not consumed: it is still readable and still
       says what it did before. */
    ql_artifact_view input_view{};
    input_view.struct_size = sizeof(input_view);
    ASSERT_EQ(QL_STATUS_OK, ql_artifact_get_view(input, &input_view, &error));
    EXPECT_EQ(sizeof(source) - 1u, input_view.size);

    /* The output was transferred, so releasing it here is correct and is the
       only release it gets. */
    ql_artifact_release(output);
    ql_artifact_release(input);
    method->destroy(instance);
    ql_plugin_unload(plugin);
}

/* An input count the method did not declare is refused before any work. The
   host enforces the arity too; the method checking as well is what keeps a
   direct caller from getting a half-processed result. */
TEST_F(ExamplePlugin, RefusesAnArityItDidNotDeclare) {
    RegistryHandle registry;
    ql_plugin_handle *plugin = nullptr;
    void *instance = nullptr;
    ql_error error{};

    ASSERT_EQ(QL_STATUS_OK,
              ql_plugin_load(registry.value, path().c_str(), &plugin, &error))
        << error.message;
    const ql_method_v1 *method =
        ql_registry_find(registry.value, "example.normalize");
    ASSERT_NE(nullptr, method);
    ASSERT_EQ(QL_STATUS_OK,
              method->create(ql_default_host(), nullptr, &instance, &error));

    EXPECT_EQ(QL_STATUS_INVALID_ARGUMENT,
              method->validate(instance, nullptr, 0u, &error));
    EXPECT_NE(nullptr, std::strstr(error.message, "exactly one"))
        << "message: " << error.message;

    method->destroy(instance);
    ql_plugin_unload(plugin);
}

/* Unloading takes the method back out of the registry before the library
   closes, so a stale lookup fails rather than jumping into unmapped code. */
TEST_F(ExamplePlugin, UnloadingRemovesTheMethodFromTheRegistry) {
    RegistryHandle registry;
    ql_plugin_handle *plugin = nullptr;
    ql_error error{};

    ASSERT_EQ(QL_STATUS_OK,
              ql_plugin_load(registry.value, path().c_str(), &plugin, &error))
        << error.message;
    ASSERT_NE(nullptr, ql_registry_find(registry.value, "example.normalize"));
    const std::size_t before = ql_registry_count(registry.value);

    ql_plugin_unload(plugin);

    EXPECT_EQ(nullptr, ql_registry_find(registry.value, "example.normalize"));
    EXPECT_EQ(before - 1u, ql_registry_count(registry.value));
}

/* The entry point is the whole ABI handshake, so it is called directly with
   hosts the plugin must refuse. Going through ql_plugin_load would only ever
   present the one host this build has; the refusal is the half of the rule
   that a plugin loaded from another release depends on, and it can only be
   reached by opening the module and calling the symbol.

   A plugin that accepted any host would turn a version mismatch from a clean
   QL_STATUS_ABI_MISMATCH into whatever happens when the host reads fields the
   plugin never wrote. */
TEST_F(ExamplePlugin, TheEntryPointRefusesAForeignHost) {
    RawModule module(path());
    ASSERT_TRUE(module.opened()) << "could not open " << path();
    const ql_plugin_init_v1_fn entry = module.entry();
    ASSERT_NE(nullptr, entry)
        << "the module does not export " << QL_PLUGIN_ENTRY_SYMBOL
        << "; QL_PLUGIN_EXPORT and hidden visibility must disagree";

    const ql_plugin_v1 *descriptor = nullptr;
    ql_error error{};

    /* The real host is accepted, and the descriptor it hands back states its
       own size and generation. */
    ASSERT_EQ(QL_STATUS_OK, entry(ql_default_host(), &descriptor, &error))
        << error.message;
    ASSERT_NE(nullptr, descriptor);
    EXPECT_EQ(QL_ABI_VERSION, descriptor->abi_version);
    EXPECT_GE(descriptor->struct_size, offsetof(ql_plugin_v1, reserved));
    EXPECT_STREQ("quodlibet-example-plugin", descriptor->name);
    EXPECT_EQ(1u, descriptor->method_count);

    /* A host from another ABI generation is refused, and the diagnostic says
       which generation this plugin was built for. */
    ql_host_v1 foreign = *ql_default_host();
    foreign.abi_version = QL_ABI_VERSION + 1u;
    descriptor = nullptr;
    EXPECT_EQ(QL_STATUS_ABI_MISMATCH,
              entry(&foreign, &descriptor, &error));
    EXPECT_EQ(nullptr, descriptor);
    EXPECT_NE(nullptr, std::strstr(error.message, "ABI"))
        << "message: " << error.message;

    /* A host too small to hold the callbacks the plugin needs is refused on
       size rather than on version, because those are different faults and a
       plugin author needs to know which one they hit. */
    ql_host_v1 truncated = *ql_default_host();
    truncated.struct_size = 1u;
    descriptor = nullptr;
    EXPECT_EQ(QL_STATUS_ABI_MISMATCH,
              entry(&truncated, &descriptor, &error));
    EXPECT_EQ(nullptr, descriptor);

    /* A null host is a programming error, not a version mismatch. */
    descriptor = nullptr;
    EXPECT_EQ(QL_STATUS_INVALID_ARGUMENT,
              entry(nullptr, &descriptor, &error));
}

}  // namespace
