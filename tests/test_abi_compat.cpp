/* The public ABI promise, tested rather than asserted in a document.

   AGENTS.MD states it in one line: public ABI structures keep `struct_size`
   and `abi_version` and are widened append-only, so the meaning and order of
   existing fields never change. Several workstreams build against these
   headers at once, which makes the promise load-bearing right now rather than
   at some future release.

   Three things follow from it, and each is checked here:

     1. A caller compiled against an OLDER, smaller version of a structure
        still works. It passes a smaller `struct_size`, and the host must read
        only the prefix that caller actually has.
     2. A structure too small to hold even the required prefix is refused,
        with a diagnostic naming what was required.
     3. Existing fields have not moved. Offsets are pinned at compile time, so
        an edit that inserts a field in the middle rather than appending to the
        end fails to build instead of silently breaking every plugin.

   Point 3 is what makes points 1 and 2 mean anything: a size check cannot
   detect a reordering. */

#include "quodlibet/method.h"
#include "quodlibet/plugin.h"
#include "quodlibet/policy.h"
#include "quodlibet/registry.h"
#include "quodlibet/solver.h"

#include <cstddef>
#include <cstring>
#include <string>
#include <type_traits>
#include <vector>

#include <gtest/gtest.h>

namespace {

/* Every public structure the plugin and solver boundaries pass across a
   compilation unit built at a different time. `struct_size` first and
   `abi_version` second is the shape every one of them must keep, because the
   host reads those two before it may trust anything else. */
template <typename T>
constexpr bool HasVersionedHeader() {
    return std::is_standard_layout_v<T> && offsetof(T, struct_size) == 0u;
}

static_assert(HasVersionedHeader<ql_method_v1>());
static_assert(HasVersionedHeader<ql_host_v1>());
static_assert(HasVersionedHeader<ql_run_context_v1>());
static_assert(HasVersionedHeader<ql_plugin_v1>());
static_assert(HasVersionedHeader<ql_solver_capability_v1>());
static_assert(HasVersionedHeader<ql_solver_check_request_v1>());
static_assert(HasVersionedHeader<ql_solver_check_result_v1>());
static_assert(HasVersionedHeader<ql_solver_descriptor_v1>());
static_assert(HasVersionedHeader<ql_policy_class_view_v1>());
static_assert(HasVersionedHeader<ql_policy_result_v1>());

/* Field order, pinned. A widening that appends leaves every one of these
   equal; one that inserts or reorders changes at least one and stops the
   build here rather than at a plugin's call site.

   The values are relative offsets rather than literals so the pins hold on
   both 32-bit and 64-bit layouts: what must not change is the sequence, not
   the byte count a particular target happens to produce. */
#define QL_PIN_ORDER(type_, first_, second_) \
    static_assert(offsetof(type_, first_) < offsetof(type_, second_))

QL_PIN_ORDER(ql_method_v1, struct_size, abi_version);
QL_PIN_ORDER(ql_method_v1, abi_version, name);
QL_PIN_ORDER(ql_method_v1, name, description);
QL_PIN_ORDER(ql_method_v1, description, output_kind);
QL_PIN_ORDER(ql_method_v1, output_kind, flags);
QL_PIN_ORDER(ql_method_v1, flags, minimum_inputs);
QL_PIN_ORDER(ql_method_v1, minimum_inputs, maximum_inputs);
QL_PIN_ORDER(ql_method_v1, maximum_inputs, create);
QL_PIN_ORDER(ql_method_v1, create, validate);
QL_PIN_ORDER(ql_method_v1, validate, run);
QL_PIN_ORDER(ql_method_v1, run, destroy);
QL_PIN_ORDER(ql_method_v1, destroy, reserved);

QL_PIN_ORDER(ql_host_v1, struct_size, abi_version);
QL_PIN_ORDER(ql_host_v1, abi_version, allocator);
QL_PIN_ORDER(ql_host_v1, allocator, artifact_create);
QL_PIN_ORDER(ql_host_v1, artifact_create, artifact_retain);
QL_PIN_ORDER(ql_host_v1, artifact_retain, artifact_release);
QL_PIN_ORDER(ql_host_v1, artifact_release, log);
QL_PIN_ORDER(ql_host_v1, log, reserved);

QL_PIN_ORDER(ql_plugin_v1, struct_size, abi_version);
QL_PIN_ORDER(ql_plugin_v1, abi_version, name);
QL_PIN_ORDER(ql_plugin_v1, name, version);
QL_PIN_ORDER(ql_plugin_v1, version, methods);
QL_PIN_ORDER(ql_plugin_v1, methods, method_count);
QL_PIN_ORDER(ql_plugin_v1, method_count, shutdown);
QL_PIN_ORDER(ql_plugin_v1, shutdown, reserved);
/* The proof-method pair is the one extension this structure has actually
   grown, and it must stay after `reserved`. A plugin built before it exists
   reports a struct_size that stops short of these two fields. */
QL_PIN_ORDER(ql_plugin_v1, reserved, proof_methods);
QL_PIN_ORDER(ql_plugin_v1, proof_methods, proof_method_count);

QL_PIN_ORDER(ql_solver_check_result_v1, struct_size, abi_version);
QL_PIN_ORDER(ql_solver_check_result_v1, abi_version, kind);
QL_PIN_ORDER(ql_solver_check_result_v1, kind, unknown_reason);
QL_PIN_ORDER(ql_solver_check_result_v1, unknown_reason, backend_name);
QL_PIN_ORDER(ql_solver_check_result_v1, backend_name, backend_version);
QL_PIN_ORDER(ql_solver_check_result_v1, backend_version, model_artifact);
QL_PIN_ORDER(ql_solver_check_result_v1, model_artifact, proof_artifact);

QL_PIN_ORDER(ql_solver_check_request_v1, struct_size, abi_version);
QL_PIN_ORDER(ql_solver_check_request_v1, abi_version, artifact_requests);
QL_PIN_ORDER(ql_solver_check_request_v1, logic, required_features);
QL_PIN_ORDER(ql_solver_check_request_v1, required_features, timeout_ms);
QL_PIN_ORDER(ql_solver_check_request_v1, timeout_ms, memory_limit_mb);
QL_PIN_ORDER(ql_solver_check_request_v1, memory_limit_mb, cancel_state);

#undef QL_PIN_ORDER

ql_status QL_CALL never_runs(void *, const ql_run_context_v1 *,
                             ql_artifact *const *, std::size_t,
                             ql_artifact **, ql_error *) {
    return QL_STATUS_METHOD_ERROR;
}

ql_method_v1 CurrentMethod(const char *name) {
    ql_method_v1 method{};
    method.struct_size = sizeof(method);
    method.abi_version = QL_ABI_VERSION;
    method.name = name;
    method.description = "an ABI compatibility fixture";
    method.flags = QL_METHOD_DETERMINISTIC;
    method.minimum_inputs = 1u;
    method.maximum_inputs = 1u;
    method.run = never_runs;
    return method;
}

struct RegistryHandle {
    ql_registry *value = nullptr;

    RegistryHandle() {
        ql_error error{};
        EXPECT_EQ(QL_STATUS_OK,
                  ql_registry_create(nullptr, &value, &error))
            << error.message;
    }
    ~RegistryHandle() { ql_registry_destroy(value); }
};

/* The smallest structure a caller may present: everything the host requires,
   and not one byte of what came later. */
constexpr std::size_t kMethodPrefix = offsetof(ql_method_v1, reserved);

/* A method descriptor living in a buffer exactly as large as it claims, so
   any read past `struct_size` runs off a real allocation rather than into
   padding a sanitizer would not notice. */
class TruncatedMethod {
public:
    TruncatedMethod(const ql_method_v1 &source, std::size_t size)
        : storage_(size) {
        ql_method_v1 copy = source;
        copy.struct_size = size;
        std::memcpy(storage_.data(), &copy,
                    size < sizeof(copy) ? size : sizeof(copy));
    }

    const ql_method_v1 *get() const {
        return reinterpret_cast<const ql_method_v1 *>(storage_.data());
    }

private:
    std::vector<unsigned char> storage_;
};

/* Point 1. A caller built before the trailing fields existed presents a
   smaller structure, and the host must take it. Refusing would mean every
   header addition broke every plugin already in the field, which is the
   opposite of what append-only is for. */
TEST(AbiCompat, AMethodFromAnOlderHeaderStillRegisters) {
    RegistryHandle registry;
    const ql_method_v1 method = CurrentMethod("abi.older-caller");
    const TruncatedMethod older(method, kMethodPrefix);
    ql_error error{};

    ASSERT_LT(kMethodPrefix, sizeof(ql_method_v1))
        << "this fixture assumes the structure has grown past its prefix";
    EXPECT_EQ(QL_STATUS_OK,
              ql_registry_register(registry.value, older.get(), &error))
        << error.message;
    EXPECT_NE(nullptr, ql_registry_find(registry.value, "abi.older-caller"));
}

/* Point 2. One byte short of the required prefix is not an old caller, it is
   a structure the host cannot read, and the diagnostic has to say what was
   required so the plugin author can act on it. */
TEST(AbiCompat, AMethodTooSmallForTheRequiredPrefixIsRefused) {
    RegistryHandle registry;
    const ql_method_v1 method = CurrentMethod("abi.too-small");
    const TruncatedMethod broken(method, kMethodPrefix - 1u);
    ql_error error{};

    EXPECT_EQ(QL_STATUS_ABI_MISMATCH,
              ql_registry_register(registry.value, broken.get(), &error));
    EXPECT_NE(nullptr, std::strstr(error.message, "ABI"))
        << "message: " << error.message;
    EXPECT_NE(nullptr, std::strstr(error.message, "abi.too-small"))
        << "message: " << error.message;
    EXPECT_EQ(nullptr, ql_registry_find(registry.value, "abi.too-small"));
}

/* A structure large enough but from a different ABI generation is refused on
   the version, not the size. The two checks are independent and a caller
   needs to know which one it failed. */
TEST(AbiCompat, AMethodFromAnotherAbiGenerationIsRefused) {
    RegistryHandle registry;
    ql_method_v1 method = CurrentMethod("abi.future");
    ql_error error{};

    method.abi_version = QL_ABI_VERSION + 1u;
    EXPECT_EQ(QL_STATUS_ABI_MISMATCH,
              ql_registry_register(registry.value, &method, &error));
    EXPECT_NE(nullptr, std::strstr(error.message, "abi.future"))
        << "message: " << error.message;
    EXPECT_EQ(nullptr, ql_registry_find(registry.value, "abi.future"));

    method.abi_version = QL_ABI_VERSION;
    EXPECT_EQ(QL_STATUS_OK,
              ql_registry_register(registry.value, &method, &error))
        << error.message;
}

/* A structure LARGER than this build knows about is an older host meeting a
   newer caller. The prefix is still readable, so the host reads what it
   understands and ignores the rest; rejecting would make every host upgrade
   mandatory the moment one plugin shipped against a newer header. */
TEST(AbiCompat, AMethodFromANewerHeaderIsReadThroughItsPrefix) {
    RegistryHandle registry;
    ql_method_v1 method = CurrentMethod("abi.newer-caller");
    ql_error error{};

    method.struct_size = sizeof(method) + 64u;
    EXPECT_EQ(QL_STATUS_OK,
              ql_registry_register(registry.value, &method, &error))
        << error.message;
    const ql_method_v1 *found =
        ql_registry_find(registry.value, "abi.newer-caller");
    ASSERT_NE(nullptr, found);
    EXPECT_STREQ("abi.newer-caller", found->name);
    EXPECT_EQ(1u, found->minimum_inputs);
}

/* The host structure a plugin receives states its own size, so a plugin built
   against an older header can tell which callbacks it may use. A host that
   understated its size would strand a plugin that legitimately has the newer
   fields. */
TEST(AbiCompat, TheDefaultHostDeclaresItsOwnSizeAndGeneration) {
    const ql_host_v1 *host = ql_default_host();

    ASSERT_NE(nullptr, host);
    EXPECT_EQ(sizeof(ql_host_v1), host->struct_size);
    EXPECT_EQ(QL_ABI_VERSION, host->abi_version);
    EXPECT_NE(nullptr, host->artifact_create);
    EXPECT_NE(nullptr, host->artifact_retain);
    EXPECT_NE(nullptr, host->artifact_release);
    EXPECT_NE(nullptr, host->log);
    EXPECT_EQ(1u, ql_allocator_is_valid(&host->allocator));
}

/* The solver descriptor's own validator applies the same two rules, so a
   backend shipped against another generation of the header cannot install
   itself by presenting a plausible-looking structure. */
TEST(AbiCompat, TheSolverDescriptorValidatorAppliesTheSameTwoRules) {
    const ql_solver_descriptor_v1 *pinned = ql_bitwuzla_solver_descriptor();
    ql_solver_descriptor_v1 descriptor = *pinned;
    ql_error error{};

    ASSERT_EQ(QL_STATUS_OK,
              ql_solver_descriptor_validate(pinned, &error))
        << error.message;

    descriptor.abi_version = QL_SOLVER_ABI_VERSION + 1u;
    EXPECT_EQ(QL_STATUS_ABI_MISMATCH,
              ql_solver_descriptor_validate(&descriptor, &error));

    descriptor = *pinned;
    descriptor.struct_size = 1u;
    EXPECT_EQ(QL_STATUS_ABI_MISMATCH,
              ql_solver_descriptor_validate(&descriptor, &error));
}

}  // namespace
