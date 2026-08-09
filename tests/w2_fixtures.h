#ifndef QUODLIBET_TESTS_W2_FIXTURES_H
#define QUODLIBET_TESTS_W2_FIXTURES_H

/* Shared helpers for the W2 exact-backend tests. They analyze one C
   translation unit, select a function, lower it, and derive the matching
   source signature so that each test states only the C source it cares
   about. */

#include "quodlibet/c_lower.h"
#include "quodlibet/signature.h"

#include <cstring>
#include <string>

#include <gtest/gtest.h>

namespace w2 {

class CFunction {
public:
    CFunction() = default;
    CFunction(const CFunction &) = delete;
    CFunction &operator=(const CFunction &) = delete;

    ~CFunction() {
        ql_artifact_release(signature_artifact_);
        ql_c_lower_result_destroy(result_);
        ql_c_frontend_unit_destroy(unit_);
    }

    ql_status Build(const char *source, const char *name, ql_error *error) {
        const std::size_t source_size = std::strlen(source);
        ql_c_function_view function{};
        ql_c_lower_result_view_v1 view{};
        ql_status status;

        source_.assign(source, source_size);
        status = ql_c_frontend_analyze(nullptr, source, source_size, &unit_,
                                       error);
        if (status != QL_STATUS_OK) {
            return status;
        }
        function.struct_size = sizeof(function);
        status = ql_c_frontend_select_function(unit_, name, std::strlen(name),
                                               &function, error);
        if (status != QL_STATUS_OK) {
            return status;
        }
        status = ql_c_lower_selected_function(nullptr, source, source_size,
                                              unit_, &function, &result_,
                                              error);
        if (status != QL_STATUS_OK) {
            return status;
        }
        view.struct_size = sizeof(view);
        status = ql_c_lower_result_get_view(result_, &view, error);
        if (status != QL_STATUS_OK) {
            return status;
        }
        support_ = view.support;
        ir_artifact_ = view.ir_artifact;
        return ql_source_signature_from_c_function(
            nullptr, unit_, &function, QL_C_DIALECT_ASM2C_GNU_V1,
            QL_TARGET_ABI_X86_64_LINUX_SYSV_LP64, &signature_artifact_,
            error);
    }

    ql_c_lower_support support() const { return support_; }
    const ql_artifact *ir_artifact() const { return ir_artifact_; }
    ql_artifact *signature_artifact() const { return signature_artifact_; }
    const std::string &source() const { return source_; }

private:
    std::string source_;
    ql_c_frontend_unit *unit_ = nullptr;
    ql_c_lower_result *result_ = nullptr;
    const ql_artifact *ir_artifact_ = nullptr;
    ql_artifact *signature_artifact_ = nullptr;
    ql_c_lower_support support_ = QL_C_LOWER_UNKNOWN;
};

/* Fails the calling test when the restricted-C slice cannot lower the source,
   because a silently skipped lowering would hide an encoding regression. */
inline void BuildOrFail(CFunction *function, const char *source,
                        const char *name) {
    ql_error error{};
    ASSERT_EQ(QL_STATUS_OK, function->Build(source, name, &error))
        << error.message;
    ASSERT_EQ(QL_C_LOWER_SUPPORTED, function->support());
    ASSERT_NE(nullptr, function->ir_artifact());
    ASSERT_NE(nullptr, function->signature_artifact());
}

}  // namespace w2

#endif
