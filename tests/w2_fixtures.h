#ifndef QUODLIBET_TESTS_W2_FIXTURES_H
#define QUODLIBET_TESTS_W2_FIXTURES_H

/* Shared helpers for the W2 exact-backend tests. They analyze one C
   translation unit, select a function, lower it, and derive the matching
   source signature so that each test states only the C source it cares
   about. */

#include "quodlibet/c_lower.h"
#include "quodlibet/problem.h"
#include "quodlibet/signature.h"
#include "quodlibet/solver.h"

#include <cstring>
#include <string>
#include <vector>

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

/* A schema v2 problem over two lowered C functions with the identity argument
   correspondence, plus both opened IRs. */
class Pair {
public:
    Pair() = default;
    Pair(const Pair &) = delete;
    Pair &operator=(const Pair &) = delete;

    ~Pair() {
        ql_ir_release(left_ir_);
        ql_ir_release(right_ir_);
        ql_problem_release(problem_);
        ql_artifact_release(artifact_);
    }

    ql_status Build(const char *left_source, const char *left_name,
                    const char *right_source, const char *right_name,
                    const ql_semantic_contract_v1 &contract,
                    ql_error *error) {
        ql_problem_definition_v2 definition{};
        std::vector<ql_problem_argument_binding_v1> bindings;
        ql_source_signature *signature = nullptr;
        ql_source_signature_view_v1 signature_view{};
        ql_status status = left_.Build(left_source, left_name, error);

        if (status != QL_STATUS_OK) {
            return status;
        }
        status = right_.Build(right_source, right_name, error);
        if (status != QL_STATUS_OK) {
            return status;
        }
        if (left_.support() != QL_C_LOWER_SUPPORTED ||
            right_.support() != QL_C_LOWER_SUPPORTED) {
            ql_error_set(error, QL_STATUS_TYPE_MISMATCH,
                         "a source is outside the restricted-C slice");
            return QL_STATUS_TYPE_MISMATCH;
        }
        status = ql_source_signature_open(nullptr, left_.signature_artifact(),
                                          &signature, error);
        if (status != QL_STATUS_OK) {
            return status;
        }
        signature_view.struct_size = sizeof(signature_view);
        status = ql_source_signature_get_view(signature, &signature_view,
                                              error);
        if (status == QL_STATUS_OK) {
            for (std::size_t index = 0u;
                 index < signature_view.argument_count; ++index) {
                ql_problem_argument_binding_v1 binding{};
                binding.struct_size = sizeof(binding);
                binding.left_index = static_cast<std::uint32_t>(index);
                binding.right_index = static_cast<std::uint32_t>(index);
                bindings.push_back(binding);
            }
        }
        ql_source_signature_release(signature);
        if (status != QL_STATUS_OK) {
            return status;
        }

        ql_problem_definition_v2_init(&definition);
        definition.contract = contract;
        definition.left_source = left_.source().c_str();
        definition.left_source_size = left_.source().size();
        definition.left_function_name = left_name;
        definition.left_function_name_size = std::strlen(left_name);
        definition.right_source = right_.source().c_str();
        definition.right_source_size = right_.source().size();
        definition.right_function_name = right_name;
        definition.right_function_name_size = std::strlen(right_name);
        definition.left_signature = left_.signature_artifact();
        definition.right_signature = right_.signature_artifact();
        definition.argument_bindings = bindings.empty() ? nullptr
                                                        : bindings.data();
        definition.argument_binding_count = bindings.size();
        status = ql_problem_artifact_create_v2(nullptr, &definition,
                                               &artifact_, error);
        if (status != QL_STATUS_OK) {
            return status;
        }
        status = ql_problem_open(nullptr, artifact_, &problem_, error);
        if (status != QL_STATUS_OK) {
            return status;
        }
        status = ql_ir_open(nullptr, left_.ir_artifact(), &left_ir_, error);
        if (status != QL_STATUS_OK) {
            return status;
        }
        return ql_ir_open(nullptr, right_.ir_artifact(), &right_ir_, error);
    }

    ql_problem *problem() const { return problem_; }
    ql_artifact *artifact() const { return artifact_; }
    ql_ir *left_ir() const { return left_ir_; }
    ql_ir *right_ir() const { return right_ir_; }

private:
    CFunction left_;
    CFunction right_;
    ql_artifact *artifact_ = nullptr;
    ql_problem *problem_ = nullptr;
    ql_ir *left_ir_ = nullptr;
    ql_ir *right_ir_ = nullptr;
};

inline ql_semantic_contract_v1 DefaultContract() {
    ql_semantic_contract_v1 contract{};
    ql_semantic_contract_init(&contract);
    return contract;
}

/* Contract observing exactly the given axes, with the memory and
   external-call mode fields kept consistent with their observation bits. */
inline ql_semantic_contract_v1 ContractObserving(std::uint64_t observations) {
    ql_semantic_contract_v1 contract = DefaultContract();
    contract.observations = observations;
    contract.memory_observation = (observations & QL_OBSERVE_MEMORY) != 0u
                                      ? QL_MEMORY_FINAL_REACHABLE_STATE
                                      : QL_MEMORY_IGNORE;
    contract.external_call_observation =
        (observations & QL_OBSERVE_EXTERNAL_CALLS) != 0u
            ? QL_EXTERNAL_CALLS_ORDERED_TRACE
            : QL_EXTERNAL_CALLS_IGNORE;
    return contract;
}

}  // namespace w2

#endif
