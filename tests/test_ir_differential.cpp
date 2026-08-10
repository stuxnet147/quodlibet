#include "quodlibet/c_lower.h"
#include "quodlibet/ir_interp.h"
#include "quodlibet/ir_verify.h"

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <functional>
#include <string>
#include <vector>

#include <gtest/gtest.h>

/* Differential testing of the lowering against real compiled execution.

   The reference is not a model of C: it is the same C function, compiled by
   the same compiler that builds this test binary, and actually run. The macro
   below emits the function and the source text the lowering receives from one
   piece of text, so the two cannot drift apart. That also removes the "clang
   was not available" caveat entirely: if this test binary exists, the
   reference compiled, so the comparison always runs.

   Two constraints follow from running the reference on the host.

   The target ABI is x86_64 Linux LP64, where `long` is 64 bits. On Windows it
   is 32. Cases therefore use only the types whose width the target ABI and
   both hosts agree on: char, short, int, unsigned, long long. `long` is
   excluded on purpose, and a case that used it would be comparing two
   different functions.

   The reference is never called on inputs whose C evaluation is undefined,
   because running undefined behaviour would make the comparison meaningless.
   Each case says which inputs are defined, and definedness for overflow comes
   from the compiler's own checked-arithmetic builtins rather than from a
   hand-derived rule. The check is two-sided: on defined inputs the
   interpreter must return the reference's exact value, and on undefined
   inputs it must report undefined behaviour rather than a value. */

#define QL_DIFF_FUNCTION(name, ...)                                            \
  extern "C" {                                                                 \
  __VA_ARGS__                                                                  \
  }                                                                            \
  static const char name##_source[] = #__VA_ARGS__

QL_DIFF_FUNCTION(sum, int diff_sum(int a, int b) { return a + b; });
QL_DIFF_FUNCTION(difference, int diff_sub(int a, int b) { return a - b; });
QL_DIFF_FUNCTION(product, int diff_mul(int a, int b) { return a * b; });
QL_DIFF_FUNCTION(quotient, int diff_div(int a, int b) { return a / b; });
QL_DIFF_FUNCTION(remainder, int diff_rem(int a, int b) { return a % b; });
QL_DIFF_FUNCTION(
    unsigned_quotient,
    unsigned int diff_udiv(unsigned int a, unsigned int b) { return a / b; });
QL_DIFF_FUNCTION(
    unsigned_wrap, unsigned int diff_wrap(unsigned int a, unsigned int b) {
      return a * b + 7u;
    });
QL_DIFF_FUNCTION(
    unsigned_shift,
    unsigned int diff_ushl(unsigned int a, int b) { return a << b; });
QL_DIFF_FUNCTION(signed_shift, int diff_shl(int a, int b) { return a << b; });
QL_DIFF_FUNCTION(right_shift, int diff_shr(int a, int b) { return a >> b; });
QL_DIFF_FUNCTION(
    bitwise, int diff_bits(int a, int b) { return (a & b) | (a ^ ~b); });
QL_DIFF_FUNCTION(compare, int diff_cmp(int a, int b) { return a < b; });
QL_DIFF_FUNCTION(
    unsigned_compare,
    int diff_ucmp(unsigned int a, unsigned int b) { return a <= b; });
QL_DIFF_FUNCTION(
    short_circuit,
    int diff_logic(int a, int b) { return b != 0 && a / b > 1; });
QL_DIFF_FUNCTION(
    short_circuit_or,
    int diff_logic_or(int a, int b) { return b == 0 || a % b == 0; });
QL_DIFF_FUNCTION(
    branches, int diff_branch(int a, int b) {
      int x;
      if (a < 3) {
        x = 0;
      } else if (a < b) {
        x = 1;
      } else {
        x = a - b;
      }
      return x;
    });
QL_DIFF_FUNCTION(narrow, short diff_short(short a, short b) { return a + b; });
QL_DIFF_FUNCTION(byte, char diff_char(char a, char b) { return a + b; });
QL_DIFF_FUNCTION(
    wide, long long diff_mul64(long long a, long long b) { return a * b; });
QL_DIFF_FUNCTION(
    mixed, long long diff_mixed(int a, unsigned int b) { return a + b; });
QL_DIFF_FUNCTION(
    cast_narrow, int diff_cast_narrow(int a, int b) { return (short)(a + b); });
QL_DIFF_FUNCTION(
    cast_unsigned, int diff_cast_unsigned(int a, int b) {
      return (unsigned int)a > (unsigned int)b;
    });
QL_DIFF_FUNCTION(
    cast_byte,
    int diff_cast_byte(int a, int b) { return (char)a + (unsigned char)b; });
QL_DIFF_FUNCTION(
    named, typedef unsigned int QL_DIFF_U32; typedef QL_DIFF_U32 QL_DIFF_WORD;
    QL_DIFF_WORD diff_named(QL_DIFF_WORD a, QL_DIFF_U32 b) {
      return a / b + 7u;
    });
QL_DIFF_FUNCTION(
    named_signed, typedef int QL_DIFF_INT;
    QL_DIFF_INT diff_named_signed(QL_DIFF_INT a, QL_DIFF_INT b) {
      return (QL_DIFF_INT)(a * b);
    });
QL_DIFF_FUNCTION(
    switch_flow, unsigned int diff_switch(unsigned int x, unsigned int seed) {
      switch (x) {
      case 0u:
        seed += 2u;
      case 1u:
        if ((seed & 1u) != 0u) {
          seed ^= 9u;
          break;
        }
        seed += 4u;
        break;
      case 5u:
        return seed + 20u;
      default:
        seed ^= 0x55u;
      }
      return seed;
    });
QL_DIFF_FUNCTION(
    goto_flow, unsigned int diff_goto(unsigned int x, unsigned int seed) {
      if ((x & 1u) != 0u) {
        seed += 3u;
        goto out;
      }
      seed ^= 5u;
      if (x == 2u)
        goto late;
      seed += 7u;
    late:
      seed ^= 11u;
    out:
      return seed;
    });
QL_DIFF_FUNCTION(
    nested_goto,
    unsigned int diff_nested_goto(unsigned int x, unsigned int seed) {
      if ((x & 1u) != 0u) {
        seed += 3u;
        goto inside;
      }
      seed ^= 5u;
      if ((x & 2u) != 0u) {
        seed += 7u;
      inside:
        seed ^= 11u;
      }
      return seed;
    });
QL_DIFF_FUNCTION(
    loop_flow, unsigned int diff_loop(unsigned int a, unsigned int seed) {
      unsigned int i = 0u;
      unsigned int n = a & 15u;
      for (; i < n; ++i) {
        if (i == 2u)
          continue;
        seed += i;
        if (seed == 19u)
          break;
      }
      while (i < n) {
        seed ^= i;
        ++i;
      }
      do {
        seed += 3u;
      } while ((seed & 3u) == 0u);
      return seed;
    });
QL_DIFF_FUNCTION(
    loop_goto, unsigned int diff_loop_goto(unsigned int a, unsigned int seed) {
      unsigned int i = 0u;
      unsigned int n = a & 15u;
      for (; i < n; ++i) {
        if ((seed ^ i) == 7u)
          goto out;
        seed += i;
      }
      seed ^= 0x55u;
    out:
      return seed + i;
    });
QL_DIFF_FUNCTION(loop_goto_shapes,
                 unsigned int diff_loop_goto_shapes(unsigned int a,
                                                     unsigned int seed) {
                   unsigned int i = 0u;
                   unsigned int n = a & 7u;
                   while (i < n) {
                     if ((seed ^ i) == 3u)
                       goto out;
                     seed += i++;
                   }
                   do {
                     if (seed + i == 21u)
                       goto out;
                     seed ^= i++;
                   } while (i < n + 2u);
                   seed += 5u;
                 out:
                   return seed + i;
                 });

namespace {

/* ------------------------------------------------------------------ */
/* raw-bit conversions                                                 */
/* ------------------------------------------------------------------ */

int8_t AsI8(uint64_t raw) { return static_cast<int8_t>(raw & 0xffu); }
int16_t AsI16(uint64_t raw) { return static_cast<int16_t>(raw & 0xffffu); }
int32_t AsI32(uint64_t raw) {
  return static_cast<int32_t>(static_cast<uint32_t>(raw));
}
uint32_t AsU32(uint64_t raw) { return static_cast<uint32_t>(raw); }
int64_t AsI64(uint64_t raw) { return static_cast<int64_t>(raw); }

uint64_t FromI8(int8_t value) { return static_cast<uint8_t>(value); }
uint64_t FromI16(int16_t value) { return static_cast<uint16_t>(value); }
uint64_t FromI32(int32_t value) { return static_cast<uint32_t>(value); }
uint64_t FromU32(uint32_t value) { return value; }
uint64_t FromI64(int64_t value) { return static_cast<uint64_t>(value); }

/* ------------------------------------------------------------------ */
/* module handling                                                     */
/* ------------------------------------------------------------------ */

class Module {
public:
  ~Module() {
    ql_ir_release(ir_);
    ql_c_lower_result_destroy(result_);
    ql_c_frontend_unit_destroy(unit_);
  }

  bool Open(const char *source, const char *name) {
    ql_c_function_view function{};
    ql_c_lower_result_view_v1 view{};
    ql_ir_verify_report_v1 report{};
    ql_error error{};
    const std::size_t source_size = std::strlen(source);

    if (ql_c_frontend_analyze(nullptr, source, source_size, &unit_, &error) !=
        QL_STATUS_OK) {
      ADD_FAILURE() << "analyze: " << error.message;
      return false;
    }
    function.struct_size = sizeof(function);
    if (ql_c_frontend_select_function(unit_, name, std::strlen(name), &function,
                                      &error) != QL_STATUS_OK) {
      ADD_FAILURE() << "select: " << error.message;
      return false;
    }
    if (ql_c_lower_selected_function(nullptr, source, source_size, unit_,
                                     &function, &result_,
                                     &error) != QL_STATUS_OK) {
      ADD_FAILURE() << "lower: " << error.message;
      return false;
    }
    view.struct_size = sizeof(view);
    if (ql_c_lower_result_get_view(result_, &view, &error) != QL_STATUS_OK ||
        view.support != QL_C_LOWER_SUPPORTED) {
      ADD_FAILURE() << "the lowering did not accept " << name;
      return false;
    }
    if (ql_ir_open(nullptr, view.ir_artifact, &ir_, &error) != QL_STATUS_OK) {
      ADD_FAILURE() << "open: " << error.message;
      return false;
    }
    report.struct_size = sizeof(report);
    if (ql_ir_verify(nullptr, ir_, &report, &error) != QL_STATUS_OK) {
      ADD_FAILURE() << "verify: " << ql_ir_verify_code_string(report.code)
                    << ": " << report.message;
      return false;
    }
    return Collect();
  }

  ql_ir *ir() const { return ir_; }
  const std::vector<ql_ir_value_id> &parameters() const { return parameters_; }
  const std::vector<uint32_t> &widths() const { return widths_; }

private:
  bool Collect() {
    ql_ir_view_v1 view{};
    ql_error error{};
    view.struct_size = sizeof(view);
    if (ql_ir_get_view(ir_, &view, &error) != QL_STATUS_OK) {
      ADD_FAILURE() << error.message;
      return false;
    }
    for (std::size_t index = 0u; index < view.value_count; ++index) {
      ql_ir_value_view_v1 value{};
      ql_ir_type_view_v1 type{};
      value.struct_size = sizeof(value);
      if (ql_ir_value_at(ir_, index, &value, &error) != QL_STATUS_OK) {
        ADD_FAILURE() << error.message;
        return false;
      }
      if (value.definition_kind != QL_IR_VALUE_PARAMETER) {
        continue;
      }
      type.struct_size = sizeof(type);
      if (ql_ir_type_at(ir_, value.type, &type, &error) != QL_STATUS_OK) {
        ADD_FAILURE() << error.message;
        return false;
      }
      parameters_.push_back(value.id);
      widths_.push_back(type.kind == QL_IR_TYPE_BOOL ? 1u : type.bit_width);
    }
    return true;
  }

  ql_c_frontend_unit *unit_ = nullptr;
  ql_c_lower_result *result_ = nullptr;
  ql_ir *ir_ = nullptr;
  std::vector<ql_ir_value_id> parameters_;
  std::vector<uint32_t> widths_;
};

std::vector<uint8_t> Encode(uint64_t value, uint32_t width) {
  std::vector<uint8_t> bytes((width + 7u) / 8u, 0u);
  for (std::size_t index = 0u; index < bytes.size() && index < 8u; ++index) {
    bytes[index] = static_cast<uint8_t>((value >> (index * 8u)) & 0xffu);
  }
  return bytes;
}

uint64_t Truncate(uint64_t value, uint32_t width) {
  if (width >= 64u) {
    return value;
  }
  return value & ((UINT64_C(1) << width) - UINT64_C(1));
}

ql_ir_interp_result_v1 Run(const Module &module,
                           const std::vector<uint64_t> &arguments) {
  std::vector<std::vector<uint8_t>> storage;
  std::vector<ql_ir_interp_input_v1> inputs;
  ql_ir_interp_result_v1 result{};
  ql_error error{};

  storage.reserve(arguments.size());
  for (std::size_t index = 0u; index < arguments.size(); ++index) {
    storage.push_back(Encode(arguments[index], module.widths()[index]));
  }
  for (std::size_t index = 0u; index < arguments.size(); ++index) {
    ql_ir_interp_input_v1 input{};
    ql_ir_interp_input_init(&input);
    input.value = module.parameters()[index];
    input.data = storage[index].data();
    input.size = storage[index].size();
    inputs.push_back(input);
  }
  result.struct_size = sizeof(result);
  EXPECT_EQ(QL_STATUS_OK,
            ql_ir_interp_run(nullptr, module.ir(), inputs.data(), inputs.size(),
                             nullptr, &result, &error))
      << error.message;
  return result;
}

uint64_t ResultBits(const ql_ir_interp_result_v1 &result) {
  uint64_t value = 0u;
  for (std::size_t index = 0u; index < result.value_size && index < 8u;
       ++index) {
    value |= static_cast<uint64_t>(result.value[index]) << (index * 8u);
  }
  return value;
}

/* ------------------------------------------------------------------ */
/* cases                                                               */
/* ------------------------------------------------------------------ */

using Predicate = std::function<bool(uint64_t, uint64_t)>;
using Reference = std::function<uint64_t(uint64_t, uint64_t)>;

struct Case {
  const char *name;
  const char *source;
  const char *function;
  uint32_t left_width;
  uint32_t right_width;
  uint32_t return_width;
  Predicate defined;
  Reference reference;
};

bool AlwaysDefined(uint64_t, uint64_t) { return true; }

/* Definedness for overflow comes from the compiler's checked arithmetic, not
   from a rule restated by hand. */
bool AddDefined(uint64_t a, uint64_t b) {
  int32_t out = 0;
  return !__builtin_add_overflow(AsI32(a), AsI32(b), &out);
}

bool SubDefined(uint64_t a, uint64_t b) {
  int32_t out = 0;
  return !__builtin_sub_overflow(AsI32(a), AsI32(b), &out);
}

bool MulDefined(uint64_t a, uint64_t b) {
  int32_t out = 0;
  return !__builtin_mul_overflow(AsI32(a), AsI32(b), &out);
}

bool Mul64Defined(uint64_t a, uint64_t b) {
  int64_t out = 0;
  return !__builtin_mul_overflow(AsI64(a), AsI64(b), &out);
}

/* The predicate must never perform the division it is deciding about: on
   x86 the INT_MIN / -1 case raises a hardware exception. */
bool SignedDivisionDefined(uint64_t a, uint64_t b) {
  return AsI32(b) != 0 && !(AsI32(a) == INT32_MIN && AsI32(b) == -1);
}

bool UnsignedDivisionDefined(uint64_t, uint64_t b) { return AsU32(b) != 0u; }

bool ShiftAmountDefined(uint64_t, uint64_t b) {
  return AsI32(b) >= 0 && AsI32(b) < 32;
}

bool SignedShiftDefined(uint64_t a, uint64_t b) {
  int64_t widened;
  if (!ShiftAmountDefined(a, b) || AsI32(a) < 0) {
    return false;
  }
  widened = static_cast<int64_t>(AsI32(a)) << AsI32(b);
  return widened >= INT32_MIN && widened <= INT32_MAX;
}

const Case kCases[] = {
    {"sum", sum_source, "diff_sum", 32u, 32u, 32u, AddDefined,
     [](uint64_t a, uint64_t b) {
       return FromI32(diff_sum(AsI32(a), AsI32(b)));
     }},
    {"difference", difference_source, "diff_sub", 32u, 32u, 32u, SubDefined,
     [](uint64_t a, uint64_t b) {
       return FromI32(diff_sub(AsI32(a), AsI32(b)));
     }},
    {"product", product_source, "diff_mul", 32u, 32u, 32u, MulDefined,
     [](uint64_t a, uint64_t b) {
       return FromI32(diff_mul(AsI32(a), AsI32(b)));
     }},
    {"quotient", quotient_source, "diff_div", 32u, 32u, 32u,
     SignedDivisionDefined,
     [](uint64_t a, uint64_t b) {
       return FromI32(diff_div(AsI32(a), AsI32(b)));
     }},
    {"remainder", remainder_source, "diff_rem", 32u, 32u, 32u,
     SignedDivisionDefined,
     [](uint64_t a, uint64_t b) {
       return FromI32(diff_rem(AsI32(a), AsI32(b)));
     }},
    {"unsigned_quotient", unsigned_quotient_source, "diff_udiv", 32u, 32u, 32u,
     UnsignedDivisionDefined,
     [](uint64_t a, uint64_t b) {
       return FromU32(diff_udiv(AsU32(a), AsU32(b)));
     }},
    {"unsigned_wrap", unsigned_wrap_source, "diff_wrap", 32u, 32u, 32u,
     AlwaysDefined,
     [](uint64_t a, uint64_t b) {
       return FromU32(diff_wrap(AsU32(a), AsU32(b)));
     }},
    {"unsigned_shift", unsigned_shift_source, "diff_ushl", 32u, 32u, 32u,
     ShiftAmountDefined,
     [](uint64_t a, uint64_t b) {
       return FromU32(diff_ushl(AsU32(a), AsI32(b)));
     }},
    {"signed_shift", signed_shift_source, "diff_shl", 32u, 32u, 32u,
     SignedShiftDefined,
     [](uint64_t a, uint64_t b) {
       return FromI32(diff_shl(AsI32(a), AsI32(b)));
     }},
    {"right_shift", right_shift_source, "diff_shr", 32u, 32u, 32u,
     ShiftAmountDefined,
     [](uint64_t a, uint64_t b) {
       return FromI32(diff_shr(AsI32(a), AsI32(b)));
     }},
    {"bitwise", bitwise_source, "diff_bits", 32u, 32u, 32u, AlwaysDefined,
     [](uint64_t a, uint64_t b) {
       return FromI32(diff_bits(AsI32(a), AsI32(b)));
     }},
    {"compare", compare_source, "diff_cmp", 32u, 32u, 32u, AlwaysDefined,
     [](uint64_t a, uint64_t b) {
       return FromI32(diff_cmp(AsI32(a), AsI32(b)));
     }},
    {"unsigned_compare", unsigned_compare_source, "diff_ucmp", 32u, 32u, 32u,
     AlwaysDefined,
     [](uint64_t a, uint64_t b) {
       return FromI32(diff_ucmp(AsU32(a), AsU32(b)));
     }},
    {"short_circuit", short_circuit_source, "diff_logic", 32u, 32u, 32u,
     [](uint64_t a, uint64_t b) {
       return AsI32(b) == 0 || SignedDivisionDefined(a, b);
     },
     [](uint64_t a, uint64_t b) {
       return FromI32(diff_logic(AsI32(a), AsI32(b)));
     }},
    {"short_circuit_or", short_circuit_or_source, "diff_logic_or", 32u, 32u,
     32u,
     [](uint64_t a, uint64_t b) {
       return AsI32(b) == 0 || SignedDivisionDefined(a, b);
     },
     [](uint64_t a, uint64_t b) {
       return FromI32(diff_logic_or(AsI32(a), AsI32(b)));
     }},
    {"branches", branches_source, "diff_branch", 32u, 32u, 32u,
     [](uint64_t a, uint64_t b) {
       return AsI32(a) < 3 || AsI32(a) < AsI32(b) || SubDefined(a, b);
     },
     [](uint64_t a, uint64_t b) {
       return FromI32(diff_branch(AsI32(a), AsI32(b)));
     }},
    {"narrow", narrow_source, "diff_short", 16u, 16u, 16u, AlwaysDefined,
     [](uint64_t a, uint64_t b) {
       return FromI16(diff_short(AsI16(a), AsI16(b)));
     }},
    {"byte", byte_source, "diff_char", 8u, 8u, 8u, AlwaysDefined,
     [](uint64_t a, uint64_t b) {
       return FromI8(diff_char(AsI8(a), AsI8(b)));
     }},
    {"wide", wide_source, "diff_mul64", 64u, 64u, 64u, Mul64Defined,
     [](uint64_t a, uint64_t b) {
       return FromI64(diff_mul64(AsI64(a), AsI64(b)));
     }},
    {"mixed", mixed_source, "diff_mixed", 32u, 32u, 64u, AlwaysDefined,
     [](uint64_t a, uint64_t b) {
       return FromI64(diff_mixed(AsI32(a), AsU32(b)));
     }},
    {"cast_narrow", cast_narrow_source, "diff_cast_narrow", 32u, 32u, 32u,
     AddDefined,
     [](uint64_t a, uint64_t b) {
       return FromI32(diff_cast_narrow(AsI32(a), AsI32(b)));
     }},
    {"cast_unsigned", cast_unsigned_source, "diff_cast_unsigned", 32u, 32u, 32u,
     AlwaysDefined,
     [](uint64_t a, uint64_t b) {
       return FromI32(diff_cast_unsigned(AsI32(a), AsI32(b)));
     }},
    {"cast_byte", cast_byte_source, "diff_cast_byte", 32u, 32u, 32u,
     AlwaysDefined,
     [](uint64_t a, uint64_t b) {
       return FromI32(diff_cast_byte(AsI32(a), AsI32(b)));
     }},
    {"named", named_source, "diff_named", 32u, 32u, 32u,
     UnsignedDivisionDefined,
     [](uint64_t a, uint64_t b) {
       return FromU32(diff_named(AsU32(a), AsU32(b)));
     }},
    {"named_signed", named_signed_source, "diff_named_signed", 32u, 32u, 32u,
     MulDefined,
     [](uint64_t a, uint64_t b) {
       return FromI32(diff_named_signed(AsI32(a), AsI32(b)));
     }},
    {"switch_flow", switch_flow_source, "diff_switch", 32u, 32u, 32u,
     AlwaysDefined,
     [](uint64_t a, uint64_t b) {
       return FromU32(diff_switch(AsU32(a), AsU32(b)));
     }},
    {"goto_flow", goto_flow_source, "diff_goto", 32u, 32u, 32u, AlwaysDefined,
     [](uint64_t a, uint64_t b) {
       return FromU32(diff_goto(AsU32(a), AsU32(b)));
     }},
    {"nested_goto", nested_goto_source, "diff_nested_goto", 32u, 32u, 32u,
     AlwaysDefined,
     [](uint64_t a, uint64_t b) {
       return FromU32(diff_nested_goto(AsU32(a), AsU32(b)));
     }},
    {"loop_flow", loop_flow_source, "diff_loop", 32u, 32u, 32u, AlwaysDefined,
     [](uint64_t a, uint64_t b) {
       return FromU32(diff_loop(AsU32(a), AsU32(b)));
     }},
    {"loop_goto", loop_goto_source, "diff_loop_goto", 32u, 32u, 32u,
     AlwaysDefined,
     [](uint64_t a, uint64_t b) {
       return FromU32(diff_loop_goto(AsU32(a), AsU32(b)));
     }},
    {"loop_goto_shapes", loop_goto_shapes_source, "diff_loop_goto_shapes",
     32u, 32u, 32u, AlwaysDefined,
     [](uint64_t a, uint64_t b) {
       return FromU32(diff_loop_goto_shapes(AsU32(a), AsU32(b)));
     }},
};

/* ------------------------------------------------------------------ */
/* input generation                                                    */
/* ------------------------------------------------------------------ */

uint64_t NextRandom(uint64_t *state) {
  uint64_t value;
  *state += UINT64_C(0x9e3779b97f4a7c15);
  value = *state;
  value = (value ^ (value >> 30)) * UINT64_C(0xbf58476d1ce4e5b9);
  value = (value ^ (value >> 27)) * UINT64_C(0x94d049bb133111eb);
  return value ^ (value >> 31);
}

/* Boundaries where signed and unsigned interpretations, shift ranges, and
   overflow thresholds meet. Random inputs almost never hit these. */
const uint64_t kEdges[] = {
    0u,
    1u,
    2u,
    3u,
    7u,
    8u,
    31u,
    32u,
    63u,
    64u,
    0x7fu,
    0x80u,
    0xffu,
    0x7fffu,
    0x8000u,
    0xffffu,
    UINT64_C(0x7fffffff),
    UINT64_C(0x80000000),
    UINT64_C(0xffffffff),
    UINT64_C(0x7fffffffffffffff),
    UINT64_C(0x8000000000000000),
    UINT64_MAX,
    UINT64_C(3037000499),
    UINT64_C(3037000500),
};

struct Counters {
  std::size_t compared = 0u;
  std::size_t undefined = 0u;
};

void CheckOne(const Case &item, const Module &module, uint64_t left,
              uint64_t right, Counters *counters) {
  const uint64_t a = Truncate(left, item.left_width);
  const uint64_t b = Truncate(right, item.right_width);
  const ql_ir_interp_result_v1 result = Run(module, {a, b});

  if (!item.defined(a, b)) {
    /* Running the reference here would be running undefined behaviour, so
       the only thing to check is that the lowering noticed. */
    EXPECT_EQ(QL_IR_INTERP_OUTCOME_UNDEFINED_BEHAVIOR, result.outcome)
        << item.name << "(" << a << ", " << b << ") is undefined in C but "
        << "the lowering produced "
        << ql_ir_interp_outcome_string(result.outcome);
    ++counters->undefined;
    return;
  }
  ASSERT_EQ(QL_IR_INTERP_OUTCOME_RETURN, result.outcome)
      << item.name << "(" << a << ", " << b << ") is defined in C but the "
      << "lowering reported " << ql_ir_interp_outcome_string(result.outcome)
      << " / " << ql_ir_interp_ub_reason_string(result.ub_reason);
  EXPECT_EQ(Truncate(item.reference(a, b), item.return_width),
            Truncate(ResultBits(result), item.return_width))
      << item.name << "(" << a << ", " << b << ")";
  ++counters->compared;
}

TEST(IrDifferential, MatchesCompiledExecutionOnEdgeValues) {
  for (const Case &item : kCases) {
    Module module;
    Counters counters;
    SCOPED_TRACE(item.name);
    ASSERT_TRUE(module.Open(item.source, item.function));
    for (uint64_t left : kEdges) {
      for (uint64_t right : kEdges) {
        CheckOne(item, module, left, right, &counters);
      }
    }
    /* Every case must exercise both sides, or the case is not testing
       what it claims to test. */
    EXPECT_GT(counters.compared, 0u);
  }
}

TEST(IrDifferential, MatchesCompiledExecutionOnRandomInputs) {
  const std::size_t kRounds = 2000u;
  for (const Case &item : kCases) {
    Module module;
    Counters counters;
    uint64_t state = UINT64_C(0x5151201042198765);
    SCOPED_TRACE(item.name);
    ASSERT_TRUE(module.Open(item.source, item.function));
    for (std::size_t round = 0u; round < kRounds; ++round) {
      uint64_t left = NextRandom(&state);
      uint64_t right = NextRandom(&state);
      /* Bias one operand small so shift and division cases spend most
         of their inputs inside the defined range instead of rejecting
         almost everything. */
      if ((round & 1u) != 0u) {
        right &= 0x3fu;
      }
      CheckOne(item, module, left, right, &counters);
    }
    EXPECT_GT(counters.compared, 0u);
  }
}

TEST(IrDifferential, ExercisesBothSidesOfEveryPartialOperation) {
  struct Expectation {
    const char *name;
    bool needs_undefined;
  };
  const Expectation expectations[] = {
      {"sum", true},
      {"difference", true},
      {"product", true},
      {"quotient", true},
      {"remainder", true},
      {"unsigned_quotient", true},
      {"unsigned_shift", true},
      {"signed_shift", true},
      {"right_shift", true},
      {"wide", true},
  };

  for (const Expectation &expectation : expectations) {
    const Case *found = nullptr;
    Module module;
    Counters counters;
    for (const Case &item : kCases) {
      if (std::strcmp(item.name, expectation.name) == 0) {
        found = &item;
        break;
      }
    }
    ASSERT_NE(nullptr, found) << expectation.name;
    SCOPED_TRACE(expectation.name);
    ASSERT_TRUE(module.Open(found->source, found->function));
    for (uint64_t left : kEdges) {
      for (uint64_t right : kEdges) {
        CheckOne(*found, module, left, right, &counters);
      }
    }
    /* A case that never reaches an undefined input proves nothing about
       the guards, so the absence is a test failure, not a silent pass. */
    EXPECT_GT(counters.undefined, 0u) << expectation.name;
    EXPECT_GT(counters.compared, 0u) << expectation.name;
  }
}

} // namespace
