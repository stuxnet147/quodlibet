/* The interpreter's memory model under the ASM2C_GNU_V1 flat address space.

   The modules here are built by hand rather than lowered, because the point
   is the model itself: which accesses are defined, what a load sees, and that
   an older memory version stays readable after a later store. The guards are
   deliberately weak (UB_GUARD on a true constant), which is what lets these
   tests show the interpreter catching an access the guard does not cover. */

#include "quodlibet/ir_interp.h"
#include "quodlibet/ir_verify.h"

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <vector>

#include <gtest/gtest.h>

namespace {

const uint64_t kBase = QL_IR_INTERP_FIRST_OBJECT_ADDRESS;

class Builder {
public:
    Builder() {
        ql_error error{};
        EXPECT_EQ(QL_STATUS_OK,
                  ql_ir_builder_create(nullptr, &builder_, &error));
    }
    ~Builder() {
        ql_ir_release(ir_);
        ql_artifact_release(artifact_);
        ql_ir_builder_destroy(builder_);
    }

    ql_ir_builder *get() const { return builder_; }

    ql_ir_type_id Type(ql_ir_type_kind kind, uint32_t width = 0u,
                       ql_ir_type_id element = QL_IR_INVALID_TYPE_ID) {
        ql_ir_type_definition_v1 definition{};
        ql_ir_type_id id = QL_IR_INVALID_TYPE_ID;
        ql_error error{};
        ql_ir_type_definition_init(&definition, kind);
        definition.bit_width = width;
        definition.element_type = element;
        EXPECT_EQ(QL_STATUS_OK,
                  ql_ir_builder_add_type(builder_, &definition, &id, &error))
            << error.message;
        return id;
    }

    ql_ir_value_id Parameter(ql_ir_type_id type, const char *name) {
        ql_ir_value_id id = QL_IR_INVALID_VALUE_ID;
        ql_error error{};
        EXPECT_EQ(QL_STATUS_OK,
                  ql_ir_builder_add_parameter(builder_, type, name,
                                              std::strlen(name), &id, &error))
            << error.message;
        parameters_.push_back(id);
        return id;
    }

    ql_ir_value_id Constant(ql_ir_type_id type,
                            const std::vector<uint8_t> &bytes) {
        ql_ir_value_id id = QL_IR_INVALID_VALUE_ID;
        ql_error error{};
        EXPECT_EQ(QL_STATUS_OK,
                  ql_ir_builder_add_constant(builder_, type, bytes.data(),
                                             bytes.size(), &id, &error))
            << error.message;
        return id;
    }

    ql_ir_block_id Block(const char *label) {
        ql_ir_block_id id = QL_IR_INVALID_BLOCK_ID;
        ql_error error{};
        EXPECT_EQ(QL_STATUS_OK,
                  ql_ir_builder_add_block(builder_, label, std::strlen(label),
                                          &id, &error))
            << error.message;
        return id;
    }

    ql_ir_value_id Append(ql_ir_block_id block, ql_ir_opcode opcode,
                          const std::vector<ql_ir_value_id> &operands,
                          ql_ir_type_id result_type = QL_IR_INVALID_TYPE_ID,
                          uint64_t effects = QL_IR_EFFECT_NONE) {
        ql_ir_instruction_definition_v1 definition{};
        ql_ir_instruction_id instruction = QL_IR_INVALID_INSTRUCTION_ID;
        ql_ir_value_id result = QL_IR_INVALID_VALUE_ID;
        ql_error error{};
        ql_ir_instruction_definition_init(&definition, opcode);
        definition.operands = operands.data();
        definition.operand_count = operands.size();
        definition.effects = effects;
        if (result_type != QL_IR_INVALID_TYPE_ID) {
            definition.result_types = &result_type;
            definition.result_count = 1u;
        }
        EXPECT_EQ(QL_STATUS_OK,
                  ql_ir_builder_append_instruction(
                      builder_, block, &definition, &instruction,
                      result_type != QL_IR_INVALID_TYPE_ID ? &result
                                                           : nullptr,
                      &error))
            << error.message;
        return result;
    }

    void Return(ql_ir_block_id block, ql_ir_value_id value,
                ql_ir_value_id memory = QL_IR_INVALID_VALUE_ID) {
        ql_ir_terminator_definition_v1 terminator{};
        ql_error error{};
        ql_ir_terminator_definition_init(&terminator,
                                         QL_IR_TERMINATOR_RETURN);
        terminator.return_value = value;
        terminator.memory = memory;
        EXPECT_EQ(QL_STATUS_OK,
                  ql_ir_builder_set_terminator(builder_, block, &terminator,
                                               &error))
            << error.message;
    }

    ql_ir *Finish() {
        ql_error error{};
        EXPECT_EQ(QL_STATUS_OK,
                  ql_ir_builder_finish(builder_, &artifact_, &error))
            << error.message;
        EXPECT_EQ(QL_STATUS_OK,
                  ql_ir_open(nullptr, artifact_, &ir_, &error))
            << error.message;
        return ir_;
    }

    const std::vector<ql_ir_value_id> &parameters() const {
        return parameters_;
    }

private:
    ql_ir_builder *builder_ = nullptr;
    ql_artifact *artifact_ = nullptr;
    ql_ir *ir_ = nullptr;
    std::vector<ql_ir_value_id> parameters_;
};

std::vector<uint8_t> Bytes(uint64_t value, std::size_t count) {
    std::vector<uint8_t> bytes(count, 0u);
    for (std::size_t index = 0u; index < count && index < 8u; ++index) {
        bytes[index] = static_cast<uint8_t>((value >> (index * 8u)) & 0xffu);
    }
    return bytes;
}

struct Outcome {
    ql_ir_interp_result_v1 result{};
    ql_status status = QL_STATUS_INTERNAL_ERROR;
};

/* Binds the parameters in table order: a memory parameter takes no bytes, and
   everything else takes its exact width. */
Outcome Execute(ql_ir *ir, const std::vector<uint64_t> &scalars,
                const std::vector<ql_ir_interp_object_v1> &objects) {
    ql_ir_view_v1 view{};
    std::vector<std::vector<uint8_t>> storage;
    std::vector<ql_ir_interp_input_v1> inputs;
    ql_ir_interp_options_v1 options{};
    ql_error error{};
    Outcome run;
    std::size_t next = 0u;

    view.struct_size = sizeof(view);
    EXPECT_EQ(QL_STATUS_OK, ql_ir_get_view(ir, &view, &error));
    for (std::size_t index = 0u; index < view.value_count; ++index) {
        ql_ir_value_view_v1 value{};
        ql_ir_type_view_v1 type{};
        value.struct_size = sizeof(value);
        EXPECT_EQ(QL_STATUS_OK, ql_ir_value_at(ir, index, &value, &error));
        if (value.definition_kind != QL_IR_VALUE_PARAMETER) {
            continue;
        }
        type.struct_size = sizeof(type);
        EXPECT_EQ(QL_STATUS_OK, ql_ir_type_at(ir, value.type, &type, &error));
        inputs.push_back(ql_ir_interp_input_v1{});
        ql_ir_interp_input_init(&inputs.back());
        inputs.back().value = value.id;
        if (type.kind == QL_IR_TYPE_MEMORY) {
            storage.push_back(std::vector<uint8_t>());
            continue;
        }
        EXPECT_LT(next, scalars.size());
        storage.push_back(Bytes(scalars[next++],
                                (type.kind == QL_IR_TYPE_BOOL
                                     ? 1u
                                     : (type.bit_width + 7u) / 8u)));
    }
    for (std::size_t index = 0u; index < inputs.size(); ++index) {
        inputs[index].data = storage[index].empty() ? nullptr
                                                    : storage[index].data();
        inputs[index].size = storage[index].size();
    }
    ql_ir_interp_options_init(&options);
    options.objects = objects.empty() ? nullptr : objects.data();
    options.object_count = objects.size();
    run.result.struct_size = sizeof(run.result);
    run.status = ql_ir_interp_run(nullptr, ir,
                                  inputs.empty() ? nullptr : inputs.data(),
                                  inputs.size(), &options, &run.result,
                                  &error);
    return run;
}

uint64_t Returned(const ql_ir_interp_result_v1 &result) {
    uint64_t value = 0u;
    for (std::size_t index = 0u; index < result.value_size && index < 8u;
         ++index) {
        value |= static_cast<uint64_t>(result.value[index]) << (index * 8u);
    }
    return value;
}

/* `int load(memory m, int *p) { return *p; }` with a guard too weak to cover
   the load, which is what lets the interpreter show an undefined access. */
struct LoadModule {
    Builder builder;
    ql_ir *ir = nullptr;

    LoadModule() {
        const ql_ir_type_id bool_type = builder.Type(QL_IR_TYPE_BOOL, 1u);
        const ql_ir_type_id bv32 =
            builder.Type(QL_IR_TYPE_BIT_VECTOR, 32u);
        const ql_ir_type_id memory = builder.Type(QL_IR_TYPE_MEMORY);
        const ql_ir_type_id pointer =
            builder.Type(QL_IR_TYPE_POINTER, 64u, bv32);
        ql_error error{};
        EXPECT_EQ(QL_STATUS_OK,
                  ql_ir_builder_set_function(builder.get(), "load", 4u, bv32,
                                             &error));
        const ql_ir_value_id m = builder.Parameter(memory, "m");
        const ql_ir_value_id p = builder.Parameter(pointer, "p");
        const ql_ir_value_id yes = builder.Constant(bool_type, {1u});
        const ql_ir_block_id entry = builder.Block("entry");
        EXPECT_EQ(QL_STATUS_OK,
                  ql_ir_builder_set_entry_block(builder.get(), entry,
                                                &error));
        /* The guard stands before the access, which is the only place a
           precondition on an address can stand. It is deliberately weak, so
           the interpreter is what catches an access it does not cover. */
        builder.Append(entry, QL_IR_OPCODE_UB_GUARD, {yes},
                       QL_IR_INVALID_TYPE_ID,
                       QL_IR_EFFECT_UNDEFINED_BEHAVIOR);
        const ql_ir_value_id loaded =
            builder.Append(entry, QL_IR_OPCODE_LOAD, {m, p}, bv32,
                           QL_IR_EFFECT_MEMORY);
        builder.Return(entry, loaded);
        ir = builder.Finish();
    }
};

std::vector<ql_ir_interp_object_v1> OneObject(const uint8_t *initial,
                                             std::size_t size,
                                             void *final_image = nullptr,
                                             uint64_t base = kBase) {
    ql_ir_interp_object_v1 object{};
    ql_ir_interp_object_init(&object);
    object.base = base;
    object.size = size;
    object.initial = initial;
    object.final_image = final_image;
    return std::vector<ql_ir_interp_object_v1>{object};
}

TEST(IrInterpMemory, ReadsTheInitialImageOfAnObject) {
    LoadModule module;
    ql_ir_verify_report_v1 report{};
    ql_error error{};
    const uint8_t initial[8] = {0x78u, 0x56u, 0x34u, 0x12u,
                                0xefu, 0xbeu, 0xadu, 0xdeu};

    report.struct_size = sizeof(report);
    ASSERT_EQ(QL_STATUS_OK, ql_ir_verify(nullptr, module.ir, &report, &error))
        << report.message;

    const Outcome first = Execute(module.ir, {kBase},
                              OneObject(initial, sizeof(initial)));
    ASSERT_EQ(QL_STATUS_OK, first.status);
    ASSERT_EQ(QL_IR_INTERP_OUTCOME_RETURN, first.result.outcome);
    EXPECT_EQ(UINT64_C(0x12345678), Returned(first.result));

    const Outcome second = Execute(module.ir, {kBase + 4u},
                               OneObject(initial, sizeof(initial)));
    ASSERT_EQ(QL_IR_INTERP_OUTCOME_RETURN, second.result.outcome);
    EXPECT_EQ(UINT64_C(0xdeadbeef), Returned(second.result));
}

TEST(IrInterpMemory, TreatsANullDereferenceAsUndefined) {
    LoadModule module;
    const uint8_t initial[4] = {1u, 0u, 0u, 0u};
    /* Nothing may occupy the first page, so address zero is in no object and
       the load is undefined however the object table is arranged. */
    const Outcome run = Execute(module.ir, {0u},
                            OneObject(initial, sizeof(initial)));
    ASSERT_EQ(QL_STATUS_OK, run.status);
    EXPECT_EQ(QL_IR_INTERP_OUTCOME_UNDEFINED_BEHAVIOR, run.result.outcome);
    /* Every guard passed, so the guard is what was too weak. */
    EXPECT_EQ(QL_IR_INTERP_UB_GUARD_INSUFFICIENT, run.result.ub_reason);
}

TEST(IrInterpMemory, RefusesAccessesThatLeaveTheObject) {
    LoadModule module;
    const uint8_t initial[8] = {0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u};

    /* The last four bytes are readable; one step further is not. */
    EXPECT_EQ(QL_IR_INTERP_OUTCOME_RETURN,
              Execute(module.ir, {kBase + 4u},
                      OneObject(initial, sizeof(initial)))
                  .result.outcome);
    EXPECT_EQ(QL_IR_INTERP_OUTCOME_UNDEFINED_BEHAVIOR,
              Execute(module.ir, {kBase + 8u},
                      OneObject(initial, sizeof(initial)))
                  .result.outcome);
    /* An access that starts inside the object but runs past its end is
       undefined as a whole; partial overlap is not partial success. */
    EXPECT_EQ(QL_IR_INTERP_OUTCOME_UNDEFINED_BEHAVIOR,
              Execute(module.ir, {kBase + 4u}, OneObject(initial, 6u))
                  .result.outcome);
}

TEST(IrInterpMemory, RequiresNaturalAlignment) {
    LoadModule module;
    const uint8_t initial[8] = {0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u};
    for (uint64_t offset = 1u; offset < 4u; ++offset) {
        SCOPED_TRACE(offset);
        EXPECT_EQ(QL_IR_INTERP_OUTCOME_UNDEFINED_BEHAVIOR,
                  Execute(module.ir, {kBase + offset},
                          OneObject(initial, sizeof(initial)))
                      .result.outcome);
    }
}

TEST(IrInterpMemory, StoresAreVisibleToLaterLoadsAndToTheFinalImage) {
    Builder builder;
    ql_error error{};
    const ql_ir_type_id bool_type = builder.Type(QL_IR_TYPE_BOOL, 1u);
    const ql_ir_type_id bv32 = builder.Type(QL_IR_TYPE_BIT_VECTOR, 32u);
    const ql_ir_type_id bv64 = builder.Type(QL_IR_TYPE_BIT_VECTOR, 64u);
    const ql_ir_type_id memory = builder.Type(QL_IR_TYPE_MEMORY);
    const ql_ir_type_id pointer = builder.Type(QL_IR_TYPE_POINTER, 64u, bv32);
    ASSERT_EQ(QL_STATUS_OK,
              ql_ir_builder_set_function(builder.get(), "bump", 4u, bv32,
                                         &error));
    const ql_ir_value_id m = builder.Parameter(memory, "m");
    const ql_ir_value_id p = builder.Parameter(pointer, "p");
    const ql_ir_value_id four = builder.Constant(bv64, Bytes(4u, 8u));
    const ql_ir_value_id one = builder.Constant(bv32, Bytes(1u, 4u));
    const ql_ir_value_id yes = builder.Constant(bool_type, {1u});
    const ql_ir_block_id entry = builder.Block("entry");
    ASSERT_EQ(QL_STATUS_OK,
              ql_ir_builder_set_entry_block(builder.get(), entry, &error));

    /* next = p + 4; m1 = store(m, next, 1); return load(m1, next) + load(m,
       next), so the older version has to stay readable after the store. */
    const ql_ir_value_id next =
        builder.Append(entry, QL_IR_OPCODE_PTR_ADD, {p, four}, pointer);
    builder.Append(entry, QL_IR_OPCODE_UB_GUARD, {yes},
                   QL_IR_INVALID_TYPE_ID, QL_IR_EFFECT_UNDEFINED_BEHAVIOR);
    const ql_ir_value_id stored =
        builder.Append(entry, QL_IR_OPCODE_STORE, {m, next, one}, memory,
                       QL_IR_EFFECT_MEMORY);
    const ql_ir_value_id fresh =
        builder.Append(entry, QL_IR_OPCODE_LOAD, {stored, next}, bv32,
                       QL_IR_EFFECT_MEMORY);
    const ql_ir_value_id stale =
        builder.Append(entry, QL_IR_OPCODE_LOAD, {m, next}, bv32,
                       QL_IR_EFFECT_MEMORY);
    const ql_ir_value_id sum =
        builder.Append(entry, QL_IR_OPCODE_ADD, {fresh, stale}, bv32);
    builder.Return(entry, sum, stored);
    ql_ir *ir = builder.Finish();
    ASSERT_NE(nullptr, ir);

    uint8_t initial[8] = {0u, 0u, 0u, 0u, 7u, 0u, 0u, 0u};
    uint8_t final_image[8] = {0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u};
    const Outcome run = Execute(ir, {kBase},
                            OneObject(initial, sizeof(initial), final_image));
    ASSERT_EQ(QL_STATUS_OK, run.status);
    ASSERT_EQ(QL_IR_INTERP_OUTCOME_RETURN, run.result.outcome);
    /* The fresh load sees 1, the stale load still sees 7. */
    EXPECT_EQ(8u, Returned(run.result));
    EXPECT_EQ(1u, final_image[4]);
    EXPECT_EQ(0u, final_image[0]);
}

TEST(IrInterpMemory, RefusesLayoutsTheModelDoesNotAdmit) {
    LoadModule module;
    const uint8_t initial[4] = {0u, 0u, 0u, 0u};

    ql_ir_interp_object_v1 low{};
    ql_ir_interp_object_init(&low);
    low.base = 0u;
    low.size = 4u;
    low.initial = initial;
    EXPECT_EQ(QL_STATUS_INVALID_ARGUMENT,
              Execute(module.ir, {kBase}, {low}).status)
        << "an object at address zero would make a null dereference defined";

    ql_ir_interp_object_v1 first{};
    ql_ir_interp_object_v1 second{};
    ql_ir_interp_object_init(&first);
    ql_ir_interp_object_init(&second);
    first.base = kBase;
    first.size = 8u;
    first.initial = initial;
    second.base = kBase + 4u;
    second.size = 8u;
    second.initial = initial;
    EXPECT_EQ(QL_STATUS_INVALID_ARGUMENT,
              Execute(module.ir, {kBase}, {first, second}).status)
        << "distinct objects must occupy disjoint byte ranges";

    ql_ir_interp_object_v1 wrapping{};
    ql_ir_interp_object_init(&wrapping);
    wrapping.base = UINT64_MAX - 2u;
    wrapping.size = 16u;
    wrapping.initial = initial;
    EXPECT_EQ(QL_STATUS_INVALID_ARGUMENT,
              Execute(module.ir, {kBase}, {wrapping}).status)
        << "an object may not wrap the address space";
}

TEST(IrInterpMemory, ReportsPointerConstructsItDoesNotModel) {
    /* PTR_ADD with an offset narrower than the pointer would need a widening
       rule, and inventing one here would put the interpreter and the SMT
       encoding on different semantics. */
    Builder builder;
    ql_error error{};
    const ql_ir_type_id bv32 = builder.Type(QL_IR_TYPE_BIT_VECTOR, 32u);
    const ql_ir_type_id memory = builder.Type(QL_IR_TYPE_MEMORY);
    const ql_ir_type_id pointer = builder.Type(QL_IR_TYPE_POINTER, 64u, bv32);
    ASSERT_EQ(QL_STATUS_OK,
              ql_ir_builder_set_function(builder.get(), "narrow", 6u, bv32,
                                         &error));
    const ql_ir_value_id m = builder.Parameter(memory, "m");
    const ql_ir_value_id p = builder.Parameter(pointer, "p");
    const ql_ir_value_id four = builder.Constant(bv32, Bytes(4u, 4u));
    const ql_ir_block_id entry = builder.Block("entry");
    ASSERT_EQ(QL_STATUS_OK,
              ql_ir_builder_set_entry_block(builder.get(), entry, &error));
    const ql_ir_value_id next =
        builder.Append(entry, QL_IR_OPCODE_PTR_ADD, {p, four}, pointer);
    const ql_ir_value_id loaded =
        builder.Append(entry, QL_IR_OPCODE_LOAD, {m, next}, bv32,
                       QL_IR_EFFECT_MEMORY);
    builder.Return(entry, loaded);
    ql_ir *ir = builder.Finish();
    ASSERT_NE(nullptr, ir);

    const uint8_t initial[8] = {0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u};
    const Outcome run = Execute(ir, {kBase}, OneObject(initial, sizeof(initial)));
    ASSERT_EQ(QL_STATUS_OK, run.status);
    EXPECT_EQ(QL_IR_INTERP_OUTCOME_UNSUPPORTED, run.result.outcome);
}

}  // namespace
