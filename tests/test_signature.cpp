#include "quodlibet/signature.h"

#include <cstddef>
#include <cstring>

#include <gtest/gtest.h>

#include "w2_fixtures.h"

namespace {

class SignatureHandle {
public:
    SignatureHandle() = default;
    SignatureHandle(const SignatureHandle &) = delete;
    SignatureHandle &operator=(const SignatureHandle &) = delete;
    ~SignatureHandle() { ql_source_signature_release(signature_); }

    ql_source_signature **output() { return &signature_; }
    ql_source_signature *get() const { return signature_; }

private:
    ql_source_signature *signature_ = nullptr;
};

class ArtifactHandle {
public:
    ArtifactHandle() = default;
    ArtifactHandle(const ArtifactHandle &) = delete;
    ArtifactHandle &operator=(const ArtifactHandle &) = delete;
    ~ArtifactHandle() { ql_artifact_release(artifact_); }

    ql_artifact **output() { return &artifact_; }
    ql_artifact *get() const { return artifact_; }

private:
    ql_artifact *artifact_ = nullptr;
};

ql_source_signature_view_v1 View(const ql_source_signature *signature) {
    ql_source_signature_view_v1 view{};
    ql_error error{};
    view.struct_size = sizeof(view);
    EXPECT_EQ(QL_STATUS_OK,
              ql_source_signature_get_view(signature, &view, &error))
        << error.message;
    return view;
}

TEST(SourceSignature, PreservesSignednessWidthAndAbiAcrossTheArtifact) {
    ql_source_signature_definition_v1 definition{};
    ql_source_type_v1 arguments[3];
    ArtifactHandle artifact;
    SignatureHandle signature;
    ql_error error{};

    ql_source_signature_definition_init(&definition);
    ql_source_type_init(&definition.return_type,
                        QL_SOURCE_TYPE_SIGNED_INTEGER);
    definition.return_type.bit_width = 32u;
    definition.function_name = "mix";
    definition.function_name_size = 3u;

    ql_source_type_init(&arguments[0], QL_SOURCE_TYPE_SIGNED_INTEGER);
    arguments[0].bit_width = 32u;
    ql_source_type_init(&arguments[1], QL_SOURCE_TYPE_UNSIGNED_INTEGER);
    arguments[1].bit_width = 32u;
    ql_source_type_init(&arguments[2], QL_SOURCE_TYPE_POINTER);
    arguments[2].bit_width = 64u;
    arguments[2].address_space = 1u;
    arguments[2].pointer_depth = 2u;
    definition.arguments = arguments;
    definition.argument_count = 3u;

    ASSERT_EQ(QL_STATUS_OK,
              ql_source_signature_artifact_create(nullptr, &definition,
                                                  artifact.output(), &error))
        << error.message;
    ASSERT_EQ(QL_STATUS_OK,
              ql_source_signature_open(nullptr, artifact.get(),
                                       signature.output(), &error))
        << error.message;

    const ql_source_signature_view_v1 view = View(signature.get());
    EXPECT_EQ(QL_SOURCE_SIGNATURE_SCHEMA_VERSION, view.schema_version);
    EXPECT_EQ(QL_C_DIALECT_ASM2C_GNU_V1, view.c_dialect);
    EXPECT_EQ(QL_TARGET_ABI_X86_64_LINUX_SYSV_LP64, view.target_abi);
    EXPECT_EQ(64u, view.pointer_width);
    EXPECT_EQ(3u, view.argument_count);
    EXPECT_EQ(QL_SOURCE_TYPE_SIGNED_INTEGER, view.return_type.kind);
    EXPECT_EQ(32u, view.return_type.bit_width);

    ql_source_type_v1 read{};
    ASSERT_EQ(QL_STATUS_OK, ql_source_signature_argument_at(
                                signature.get(), 1u, &read, &error));
    EXPECT_EQ(QL_SOURCE_TYPE_UNSIGNED_INTEGER, read.kind);
    EXPECT_EQ(32u, read.bit_width);
    ASSERT_EQ(QL_STATUS_OK, ql_source_signature_argument_at(
                                signature.get(), 2u, &read, &error));
    EXPECT_EQ(QL_SOURCE_TYPE_POINTER, read.kind);
    EXPECT_EQ(64u, read.bit_width);
    EXPECT_EQ(1u, read.address_space);
    EXPECT_EQ(2u, read.pointer_depth);
    EXPECT_EQ(QL_STATUS_NOT_FOUND, ql_source_signature_argument_at(
                                       signature.get(), 3u, &read, &error));
}

TEST(SourceSignature, SameWidthSignedAndUnsignedArgumentsAreNotInterchangeable) {
    ql_source_type_v1 left{};
    ql_source_type_v1 right{};
    ql_error error{};

    ql_source_type_init(&left, QL_SOURCE_TYPE_SIGNED_INTEGER);
    left.bit_width = 32u;
    ql_source_type_init(&right, QL_SOURCE_TYPE_UNSIGNED_INTEGER);
    right.bit_width = 32u;

    EXPECT_EQ(QL_STATUS_TYPE_MISMATCH,
              ql_source_type_compatible(&left, &right, &error));
    EXPECT_EQ(QL_STATUS_OK, ql_source_type_compatible(&left, &left, &error));
}

TEST(SourceSignature, DerivesFixedWidthsFromTheFrozenDialectTable) {
    w2::CFunction function;
    SignatureHandle signature;
    ql_error error{};

    ASSERT_NO_FATAL_FAILURE(w2::BuildOrFail(
        &function,
        "unsigned long widen(unsigned char a, short b, _Bool c)"
        "{ return a + b + c; }",
        "widen"));
    ASSERT_EQ(QL_STATUS_OK,
              ql_source_signature_open(nullptr, function.signature_artifact(),
                                       signature.output(), &error))
        << error.message;

    const ql_source_signature_view_v1 view = View(signature.get());
    ASSERT_EQ(3u, view.argument_count);
    EXPECT_EQ(QL_SOURCE_TYPE_UNSIGNED_INTEGER, view.return_type.kind);
    EXPECT_EQ(64u, view.return_type.bit_width);

    ql_source_type_v1 argument{};
    ASSERT_EQ(QL_STATUS_OK, ql_source_signature_argument_at(
                                signature.get(), 0u, &argument, &error));
    EXPECT_EQ(QL_SOURCE_TYPE_UNSIGNED_INTEGER, argument.kind);
    EXPECT_EQ(8u, argument.bit_width);
    ASSERT_EQ(QL_STATUS_OK, ql_source_signature_argument_at(
                                signature.get(), 1u, &argument, &error));
    EXPECT_EQ(QL_SOURCE_TYPE_SIGNED_INTEGER, argument.kind);
    EXPECT_EQ(16u, argument.bit_width);
    ASSERT_EQ(QL_STATUS_OK, ql_source_signature_argument_at(
                                signature.get(), 2u, &argument, &error));
    EXPECT_EQ(QL_SOURCE_TYPE_BOOL, argument.kind);
    EXPECT_EQ(1u, argument.bit_width);
}

TEST(SourceSignature, BindsToTheLoweredIrAndRejectsAMismatchedFunction) {
    w2::CFunction first;
    w2::CFunction second;
    SignatureHandle signature;
    ql_ir *first_ir = nullptr;
    ql_ir *second_ir = nullptr;
    ql_error error{};

    ASSERT_NO_FATAL_FAILURE(w2::BuildOrFail(
        &first, "int add(int x, int y){ return x + y; }", "add"));
    ASSERT_NO_FATAL_FAILURE(w2::BuildOrFail(
        &second, "int add(unsigned x, int y){ return x + y; }", "add"));
    ASSERT_EQ(QL_STATUS_OK,
              ql_source_signature_open(nullptr, first.signature_artifact(),
                                       signature.output(), &error))
        << error.message;
    ASSERT_EQ(QL_STATUS_OK,
              ql_ir_open(nullptr, first.ir_artifact(), &first_ir, &error))
        << error.message;
    ASSERT_EQ(QL_STATUS_OK,
              ql_ir_open(nullptr, second.ir_artifact(), &second_ir, &error))
        << error.message;

    EXPECT_EQ(QL_STATUS_OK,
              ql_source_signature_bind_ir(signature.get(), first_ir, &error))
        << error.message;
    /* Both IRs use 32-bit bit vectors; only the signature distinguishes the
       unsigned first parameter, so the binding must still hold here. */
    EXPECT_EQ(QL_STATUS_OK,
              ql_source_signature_bind_ir(signature.get(), second_ir, &error));

    ql_ir_release(first_ir);
    ql_ir_release(second_ir);
}

TEST(SourceSignature, RejectsWidthDisagreementBetweenSignatureAndIr) {
    w2::CFunction function;
    SignatureHandle signature;
    ql_ir *ir = nullptr;
    ql_error error{};

    ASSERT_NO_FATAL_FAILURE(w2::BuildOrFail(
        &function, "int narrow(short x){ return x; }", "narrow"));
    ASSERT_EQ(QL_STATUS_OK,
              ql_ir_open(nullptr, function.ir_artifact(), &ir, &error))
        << error.message;

    ql_source_signature_definition_v1 definition{};
    ql_source_type_v1 argument{};
    ArtifactHandle artifact;
    ql_source_signature_definition_init(&definition);
    ql_source_type_init(&definition.return_type,
                        QL_SOURCE_TYPE_SIGNED_INTEGER);
    definition.return_type.bit_width = 32u;
    definition.function_name = "narrow";
    definition.function_name_size = 6u;
    ql_source_type_init(&argument, QL_SOURCE_TYPE_SIGNED_INTEGER);
    argument.bit_width = 32u;
    definition.arguments = &argument;
    definition.argument_count = 1u;
    ASSERT_EQ(QL_STATUS_OK,
              ql_source_signature_artifact_create(nullptr, &definition,
                                                  artifact.output(), &error))
        << error.message;
    ASSERT_EQ(QL_STATUS_OK,
              ql_source_signature_open(nullptr, artifact.get(),
                                       signature.output(), &error))
        << error.message;

    EXPECT_EQ(QL_STATUS_TYPE_MISMATCH,
              ql_source_signature_bind_ir(signature.get(), ir, &error));
    ql_ir_release(ir);
}

TEST(SourceSignature, TypeChecksAPreconditionAgainstItsOwnArguments) {
    constexpr char json[] =
        "{\"schema_version\":1,\"expression\":{\"op\":\"ule\","
        "\"left\":{\"op\":\"arg\",\"index\":0},"
        "\"right\":{\"op\":\"int\",\"signed\":false,\"width\":32,"
        "\"value\":\"4096\"}}}";
    w2::CFunction function;
    SignatureHandle signature;
    ql_signature_view_v1 precondition_signature{};
    ql_signature_argument_v1 storage[QL_SOURCE_SIGNATURE_MAX_ARGUMENTS];
    ql_precondition *precondition = nullptr;
    ql_error error{};

    ASSERT_NO_FATAL_FAILURE(w2::BuildOrFail(
        &function, "unsigned clamp(unsigned n){ return n; }", "clamp"));
    ASSERT_EQ(QL_STATUS_OK,
              ql_source_signature_open(nullptr, function.signature_artifact(),
                                       signature.output(), &error))
        << error.message;
    ASSERT_EQ(QL_STATUS_OK,
              ql_source_signature_precondition_view(
                  signature.get(), &precondition_signature, storage,
                  QL_SOURCE_SIGNATURE_MAX_ARGUMENTS, &error))
        << error.message;
    ASSERT_EQ(QL_STATUS_OK,
              ql_precondition_parse(nullptr, json, sizeof(json) - 1u,
                                    &precondition_signature, &precondition,
                                    &error))
        << error.message;
    ql_precondition_destroy(precondition);
}

TEST(SourceSignature, RejectsUnknownTypeSpellingsInsteadOfGuessing) {
    w2::CFunction function;
    ql_error error{};

    EXPECT_EQ(QL_STATUS_TYPE_MISMATCH,
              function.Build("double scale(double x){ return x; }", "scale",
                             &error));
}

/* The corpus this profile serves spells every scalar through a typedef chain,
   so a signature that refuses those names describes almost nothing the
   lowering accepts. */
TEST(SourceSignature, ResolvesATypedefChainThisUnitDeclares) {
    w2::CFunction function;
    SignatureHandle signature;
    ql_source_type_v1 argument{};
    ql_error error{};

    ASSERT_NO_FATAL_FAILURE(w2::BuildOrFail(
        &function,
        "typedef unsigned char TYP_0;"
        " typedef TYP_0 TYP_1;"
        " typedef int TYP_2;"
        " TYP_2 FUN_0(TYP_1 ARG_0){ return ARG_0; }",
        "FUN_0"));
    ASSERT_EQ(QL_STATUS_OK,
              ql_source_signature_open(nullptr, function.signature_artifact(),
                                       signature.output(), &error))
        << error.message;

    const ql_source_signature_view_v1 view = View(signature.get());
    EXPECT_EQ(QL_SOURCE_TYPE_SIGNED_INTEGER, view.return_type.kind);
    EXPECT_EQ(32u, view.return_type.bit_width);
    ASSERT_EQ(1u, view.argument_count);
    ASSERT_EQ(QL_STATUS_OK, ql_source_signature_argument_at(
                                signature.get(), 0u, &argument, &error));
    EXPECT_EQ(QL_SOURCE_TYPE_UNSIGNED_INTEGER, argument.kind);
    EXPECT_EQ(8u, argument.bit_width);
}

TEST(SourceSignature, ATypedefOfAPointerIsAPointerArgument) {
    w2::CFunction function;
    SignatureHandle signature;
    ql_source_type_v1 argument{};
    ql_error error{};

    ASSERT_NO_FATAL_FAILURE(w2::BuildOrFail(
        &function,
        "typedef int TYP_0;"
        " int FUN_0(TYP_0 *ARG_0){ return ARG_0[0]; }",
        "FUN_0"));
    ASSERT_EQ(QL_STATUS_OK,
              ql_source_signature_open(nullptr, function.signature_artifact(),
                                       signature.output(), &error))
        << error.message;
    ASSERT_EQ(QL_STATUS_OK, ql_source_signature_argument_at(
                                signature.get(), 0u, &argument, &error));
    EXPECT_EQ(QL_SOURCE_TYPE_POINTER, argument.kind);
    EXPECT_EQ(64u, argument.bit_width);
}

/* A name nobody declared has no meaning to recover, and inventing one would be
   a guess about a type. */
TEST(SourceSignature, RefusesATypedefNameThisUnitNeverDeclared) {
    ql_c_frontend_unit *unit = nullptr;
    ql_c_function_view function{};
    ql_artifact *artifact = nullptr;
    constexpr char source[] = "TYP_9 FUN_0(int ARG_0){ return ARG_0; }";
    ql_error error{};

    ASSERT_EQ(QL_STATUS_OK,
              ql_c_frontend_analyze(nullptr, source, sizeof(source) - 1u,
                                    &unit, &error));
    function.struct_size = sizeof(function);
    ASSERT_EQ(QL_STATUS_OK,
              ql_c_frontend_select_function(unit, "FUN_0", 5u, &function,
                                            &error));
    EXPECT_EQ(QL_STATUS_TYPE_MISMATCH,
              ql_source_signature_from_c_function_v2(
                  nullptr, unit, &function, source, sizeof(source) - 1u,
                  QL_C_DIALECT_ASM2C_GNU_V1,
                  QL_TARGET_ABI_X86_64_LINUX_SYSV_LP64, &artifact, &error));
    EXPECT_NE(nullptr, std::strstr(error.message, "TYP_9"));
    EXPECT_EQ(nullptr, artifact);
    ql_c_frontend_unit_destroy(unit);
}

/* The v1 form is not given the source, so it cannot resolve anything. It stays
   as it was rather than quietly changing meaning for its existing callers. */
TEST(SourceSignature, TheFormWithoutSourceStillRefusesATypedefName) {
    ql_c_frontend_unit *unit = nullptr;
    ql_c_function_view function{};
    ql_artifact *artifact = nullptr;
    constexpr char source[] =
        "typedef int TYP_0; TYP_0 FUN_0(TYP_0 ARG_0){ return ARG_0; }";
    ql_error error{};

    ASSERT_EQ(QL_STATUS_OK,
              ql_c_frontend_analyze(nullptr, source, sizeof(source) - 1u,
                                    &unit, &error));
    function.struct_size = sizeof(function);
    ASSERT_EQ(QL_STATUS_OK,
              ql_c_frontend_select_function(unit, "FUN_0", 5u, &function,
                                            &error));
    EXPECT_EQ(QL_STATUS_TYPE_MISMATCH,
              ql_source_signature_from_c_function(
                  nullptr, unit, &function, QL_C_DIALECT_ASM2C_GNU_V1,
                  QL_TARGET_ABI_X86_64_LINUX_SYSV_LP64, &artifact, &error));
    EXPECT_EQ(QL_STATUS_OK,
              ql_source_signature_from_c_function_v2(
                  nullptr, unit, &function, source, sizeof(source) - 1u,
                  QL_C_DIALECT_ASM2C_GNU_V1,
                  QL_TARGET_ABI_X86_64_LINUX_SYSV_LP64, &artifact, &error))
        << error.message;
    ql_artifact_release(artifact);
    ql_c_frontend_unit_destroy(unit);
}

/* Resolution is a second derivation, not a restatement of the lowering's, so
   the binding check still has something to check. */
TEST(SourceSignature, AResolvedTypedefStillBindsToTheLoweredIr) {
    w2::CFunction function;
    SignatureHandle signature;
    ql_ir *ir = nullptr;
    ql_error error{};

    ASSERT_NO_FATAL_FAILURE(w2::BuildOrFail(
        &function,
        "typedef short TYP_0;"
        " TYP_0 FUN_0(TYP_0 ARG_0){ return ARG_0; }",
        "FUN_0"));
    ASSERT_EQ(QL_STATUS_OK,
              ql_source_signature_open(nullptr, function.signature_artifact(),
                                       signature.output(), &error))
        << error.message;
    ASSERT_EQ(QL_STATUS_OK,
              ql_ir_open(nullptr, function.ir_artifact(), &ir, &error))
        << error.message;
    EXPECT_EQ(QL_STATUS_OK,
              ql_source_signature_bind_ir(signature.get(), ir, &error))
        << error.message;
    ql_ir_release(ir);
}

}  // namespace
