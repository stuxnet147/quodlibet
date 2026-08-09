#include "quodlibet/aig.h"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <set>
#include <string>
#include <vector>

#include <gtest/gtest.h>

namespace {

class Graph {
public:
    Graph() {
        ql_error error{};
        EXPECT_EQ(QL_STATUS_OK, ql_aig_create(nullptr, &aig_, &error))
            << error.message;
    }
    Graph(const Graph &) = delete;
    Graph &operator=(const Graph &) = delete;
    ~Graph() { ql_aig_destroy(aig_); }

    ql_aig *get() const { return aig_; }
    operator ql_aig *() const { return aig_; }

private:
    ql_aig *aig_ = nullptr;
};

using Word = std::vector<ql_aig_lit>;

Word MakeInput(ql_aig *aig, std::uint32_t width) {
    Word word(width, QL_AIG_LIT_INVALID);
    ql_error error{};
    EXPECT_EQ(QL_STATUS_OK,
              ql_aig_bv_input(aig, width, word.data(), &error))
        << error.message;
    return word;
}

/* Evaluates a whole word on one concrete assignment, least significant bit
   first, so a test can compare a circuit against ordinary C arithmetic. */
std::uint32_t Evaluate(const ql_aig *aig, const Word &word,
                       const std::vector<std::uint8_t> &inputs) {
    std::uint32_t value = 0u;
    for (std::size_t index = 0u; index < word.size(); ++index) {
        std::uint32_t bit = 0u;
        ql_error error{};
        EXPECT_EQ(QL_STATUS_OK,
                  ql_aig_evaluate(aig, inputs.data(), inputs.size(),
                                  word[index], &bit, &error))
            << error.message;
        value |= bit << index;
    }
    return value;
}

std::uint32_t EvaluateBit(const ql_aig *aig, ql_aig_lit literal,
                          const std::vector<std::uint8_t> &inputs) {
    std::uint32_t bit = 0u;
    ql_error error{};
    EXPECT_EQ(QL_STATUS_OK,
              ql_aig_evaluate(aig, inputs.data(), inputs.size(), literal, &bit,
                              &error))
        << error.message;
    return bit;
}

/* The two operand words occupy inputs 0..width-1 and width..2*width-1, so an
   exhaustive sweep is just a pair of counters. */
std::vector<std::uint8_t> Assignment(std::uint32_t left, std::uint32_t right,
                                     std::uint32_t width) {
    std::vector<std::uint8_t> values;
    for (std::uint32_t index = 0u; index < width; ++index) {
        values.push_back(static_cast<std::uint8_t>((left >> index) & 1u));
    }
    for (std::uint32_t index = 0u; index < width; ++index) {
        values.push_back(static_cast<std::uint8_t>((right >> index) & 1u));
    }
    return values;
}

std::int32_t Signed(std::uint32_t value, std::uint32_t width) {
    const std::uint32_t sign = 1u << (width - 1u);
    return (value & sign) != 0u
               ? static_cast<std::int32_t>(value) -
                     static_cast<std::int32_t>(sign << 1u)
               : static_cast<std::int32_t>(value);
}

/* --- Literal algebra ------------------------------------------------------ */

TEST(Aig, ComplementIsFreeAndConstantsFold) {
    Graph aig;
    ql_aig_lit input = QL_AIG_LIT_INVALID;
    ql_aig_lit result = QL_AIG_LIT_INVALID;
    ql_aig_view_v1 view{};
    ql_error error{};

    EXPECT_EQ(QL_AIG_LIT_TRUE, ql_aig_not(QL_AIG_LIT_FALSE));
    EXPECT_EQ(QL_AIG_LIT_FALSE, ql_aig_not(QL_AIG_LIT_TRUE));
    EXPECT_EQ(1u, ql_aig_is_constant(QL_AIG_LIT_TRUE));

    ASSERT_EQ(QL_STATUS_OK, ql_aig_add_input(aig, &input, &error));
    EXPECT_EQ(0u, ql_aig_is_constant(input));
    EXPECT_EQ(0u, ql_aig_input_index(aig, input));

    ASSERT_EQ(QL_STATUS_OK,
              ql_aig_and(aig, input, QL_AIG_LIT_FALSE, &result, &error));
    EXPECT_EQ(QL_AIG_LIT_FALSE, result);
    ASSERT_EQ(QL_STATUS_OK,
              ql_aig_and(aig, input, QL_AIG_LIT_TRUE, &result, &error));
    EXPECT_EQ(input, result);
    ASSERT_EQ(QL_STATUS_OK, ql_aig_and(aig, input, input, &result, &error));
    EXPECT_EQ(input, result);
    ASSERT_EQ(QL_STATUS_OK,
              ql_aig_and(aig, input, ql_aig_not(input), &result, &error));
    EXPECT_EQ(QL_AIG_LIT_FALSE, result);

    view.struct_size = sizeof(view);
    ASSERT_EQ(QL_STATUS_OK, ql_aig_get_view(aig, &view, &error));
    /* Not one AND node was allocated for any of those four. */
    EXPECT_EQ(0u, view.and_count);
    EXPECT_EQ(1u, view.input_count);
    EXPECT_EQ(4u, view.folded_count);
}

TEST(Aig, StructurallyIdenticalNodesAreShared) {
    Graph aig;
    ql_aig_lit a = QL_AIG_LIT_INVALID;
    ql_aig_lit b = QL_AIG_LIT_INVALID;
    ql_aig_lit first = QL_AIG_LIT_INVALID;
    ql_aig_lit second = QL_AIG_LIT_INVALID;
    ql_aig_view_v1 view{};
    ql_error error{};

    ASSERT_EQ(QL_STATUS_OK, ql_aig_add_input(aig, &a, &error));
    ASSERT_EQ(QL_STATUS_OK, ql_aig_add_input(aig, &b, &error));
    ASSERT_EQ(QL_STATUS_OK, ql_aig_and(aig, a, b, &first, &error));
    /* Operand order must not create a second node. */
    ASSERT_EQ(QL_STATUS_OK, ql_aig_and(aig, b, a, &second, &error));
    EXPECT_EQ(first, second);

    view.struct_size = sizeof(view);
    ASSERT_EQ(QL_STATUS_OK, ql_aig_get_view(aig, &view, &error));
    EXPECT_EQ(1u, view.and_count);
    EXPECT_EQ(1u, view.shared_count);
}

TEST(Aig, RefusesALiteralFromAnotherGraphAndHonoursTheNodeLimit) {
    Graph first;
    Graph second;
    ql_aig_lit a = QL_AIG_LIT_INVALID;
    ql_aig_lit b = QL_AIG_LIT_INVALID;
    ql_aig_lit result = QL_AIG_LIT_INVALID;
    Word word(8u, QL_AIG_LIT_INVALID);
    ql_error error{};

    ASSERT_EQ(QL_STATUS_OK, ql_aig_add_input(first, &a, &error));
    ASSERT_EQ(QL_STATUS_OK, ql_aig_add_input(second, &b, &error));
    /* `second` has two nodes, so a literal built in `first` beyond that is not
       addressable there. */
    for (int index = 0; index < 8; ++index) {
        ASSERT_EQ(QL_STATUS_OK, ql_aig_add_input(first, &a, &error));
    }
    EXPECT_EQ(QL_STATUS_INVALID_ARGUMENT,
              ql_aig_and(second, a, b, &result, &error));

    Graph bounded;
    ql_aig_set_node_limit(bounded, 4u);
    EXPECT_EQ(QL_STATUS_METHOD_ERROR,
              ql_aig_bv_input(bounded, 8u, word.data(), &error));
    EXPECT_NE(nullptr, std::strstr(error.message, "node limit"));
}

/* --- Bit-vector semantics ------------------------------------------------- */

/* One graph, one exhaustive sweep, every scalar operation checked against
   ordinary C on the same two operands. Four bits keeps the sweep at 256 pairs
   while still exercising the sign bit, the carry chain, and the divider. */
TEST(AigBitVector, MatchesCArithmeticExhaustivelyAtFourBits) {
    constexpr std::uint32_t kWidth = 4u;
    constexpr std::uint32_t kMask = (1u << kWidth) - 1u;
    Graph aig;
    ql_error error{};

    const Word a = MakeInput(aig, kWidth);
    const Word b = MakeInput(aig, kWidth);
    Word sum(kWidth), difference(kWidth), product(kWidth), negation(kWidth);
    Word conjunction(kWidth), disjunction(kWidth), exclusive(kWidth);
    Word complement(kWidth);
    Word quotient(kWidth), remainder(kWidth);
    Word signed_quotient(kWidth), signed_remainder(kWidth);
    Word left_shift(kWidth), logical_shift(kWidth), arithmetic_shift(kWidth);
    Word selected(kWidth);
    ql_aig_lit equal = QL_AIG_LIT_INVALID;
    ql_aig_lit below = QL_AIG_LIT_INVALID;
    ql_aig_lit below_or_equal = QL_AIG_LIT_INVALID;
    ql_aig_lit signed_below = QL_AIG_LIT_INVALID;
    ql_aig_lit signed_below_or_equal = QL_AIG_LIT_INVALID;
    ql_aig_lit any_bit = QL_AIG_LIT_INVALID;
    ql_aig_lit all_bits = QL_AIG_LIT_INVALID;

    ASSERT_EQ(QL_STATUS_OK, ql_aig_bv_add(aig, a.data(), b.data(),
                                          QL_AIG_LIT_FALSE, kWidth,
                                          sum.data(), nullptr, &error))
        << error.message;
    ASSERT_EQ(QL_STATUS_OK, ql_aig_bv_sub(aig, a.data(), b.data(), kWidth,
                                          difference.data(), &error));
    ASSERT_EQ(QL_STATUS_OK, ql_aig_bv_mul(aig, a.data(), b.data(), kWidth,
                                          product.data(), &error));
    ASSERT_EQ(QL_STATUS_OK,
              ql_aig_bv_neg(aig, a.data(), kWidth, negation.data(), &error));
    ASSERT_EQ(QL_STATUS_OK, ql_aig_bv_and(aig, a.data(), b.data(), kWidth,
                                          conjunction.data(), &error));
    ASSERT_EQ(QL_STATUS_OK, ql_aig_bv_or(aig, a.data(), b.data(), kWidth,
                                         disjunction.data(), &error));
    ASSERT_EQ(QL_STATUS_OK, ql_aig_bv_xor(aig, a.data(), b.data(), kWidth,
                                          exclusive.data(), &error));
    ASSERT_EQ(QL_STATUS_OK, ql_aig_bv_not(aig, a.data(), kWidth,
                                          complement.data(), &error));
    ASSERT_EQ(QL_STATUS_OK,
              ql_aig_bv_udivrem(aig, a.data(), b.data(), kWidth,
                                quotient.data(), remainder.data(), &error));
    ASSERT_EQ(QL_STATUS_OK,
              ql_aig_bv_sdivrem(aig, a.data(), b.data(), kWidth,
                                signed_quotient.data(),
                                signed_remainder.data(), &error));
    ASSERT_EQ(QL_STATUS_OK, ql_aig_bv_shl(aig, a.data(), b.data(), kWidth,
                                          left_shift.data(), &error));
    ASSERT_EQ(QL_STATUS_OK, ql_aig_bv_lshr(aig, a.data(), b.data(), kWidth,
                                           logical_shift.data(), &error));
    ASSERT_EQ(QL_STATUS_OK, ql_aig_bv_ashr(aig, a.data(), b.data(), kWidth,
                                           arithmetic_shift.data(), &error));
    ASSERT_EQ(QL_STATUS_OK, ql_aig_bv_mux(aig, a[0], a.data(), b.data(),
                                          kWidth, selected.data(), &error));
    ASSERT_EQ(QL_STATUS_OK, ql_aig_bv_eq(aig, a.data(), b.data(), kWidth,
                                         &equal, &error));
    ASSERT_EQ(QL_STATUS_OK, ql_aig_bv_ult(aig, a.data(), b.data(), kWidth,
                                          &below, &error));
    ASSERT_EQ(QL_STATUS_OK, ql_aig_bv_ule(aig, a.data(), b.data(), kWidth,
                                          &below_or_equal, &error));
    ASSERT_EQ(QL_STATUS_OK, ql_aig_bv_slt(aig, a.data(), b.data(), kWidth,
                                          &signed_below, &error));
    ASSERT_EQ(QL_STATUS_OK, ql_aig_bv_sle(aig, a.data(), b.data(), kWidth,
                                          &signed_below_or_equal, &error));
    ASSERT_EQ(QL_STATUS_OK, ql_aig_bv_reduce_or(aig, a.data(), kWidth,
                                                &any_bit, &error));
    ASSERT_EQ(QL_STATUS_OK, ql_aig_bv_reduce_and(aig, a.data(), kWidth,
                                                 &all_bits, &error));

    for (std::uint32_t left = 0u; left <= kMask; ++left) {
        for (std::uint32_t right = 0u; right <= kMask; ++right) {
            const std::vector<std::uint8_t> values =
                Assignment(left, right, kWidth);
            const std::int32_t signed_left = Signed(left, kWidth);
            const std::int32_t signed_right = Signed(right, kWidth);

            EXPECT_EQ((left + right) & kMask, Evaluate(aig, sum, values))
                << left << " + " << right;
            EXPECT_EQ((left - right) & kMask,
                      Evaluate(aig, difference, values));
            EXPECT_EQ((left * right) & kMask, Evaluate(aig, product, values));
            EXPECT_EQ((0u - left) & kMask, Evaluate(aig, negation, values));
            EXPECT_EQ(left & right, Evaluate(aig, conjunction, values));
            EXPECT_EQ(left | right, Evaluate(aig, disjunction, values));
            EXPECT_EQ(left ^ right, Evaluate(aig, exclusive, values));
            EXPECT_EQ((~left) & kMask, Evaluate(aig, complement, values));

            /* Division by zero is totalized, not undefined: SMT-LIB says the
               quotient is all ones and the remainder is the dividend. */
            EXPECT_EQ(right == 0u ? kMask : left / right,
                      Evaluate(aig, quotient, values))
                << left << " / " << right;
            EXPECT_EQ(right == 0u ? left : left % right,
                      Evaluate(aig, remainder, values));

            const std::uint32_t expected_sdiv =
                signed_right == 0
                    ? (signed_left >= 0 ? kMask : 1u)
                    : (static_cast<std::uint32_t>(signed_left / signed_right) &
                       kMask);
            const std::uint32_t expected_srem =
                signed_right == 0
                    ? left
                    : (static_cast<std::uint32_t>(signed_left % signed_right) &
                       kMask);
            EXPECT_EQ(expected_sdiv, Evaluate(aig, signed_quotient, values))
                << signed_left << " / " << signed_right;
            EXPECT_EQ(expected_srem, Evaluate(aig, signed_remainder, values));

            EXPECT_EQ(right >= kWidth ? 0u : (left << right) & kMask,
                      Evaluate(aig, left_shift, values))
                << left << " << " << right;
            EXPECT_EQ(right >= kWidth ? 0u : left >> right,
                      Evaluate(aig, logical_shift, values));
            const std::uint32_t expected_ashr =
                right >= kWidth
                    ? (signed_left < 0 ? kMask : 0u)
                    : (static_cast<std::uint32_t>(signed_left >>
                                                  static_cast<int>(right)) &
                       kMask);
            EXPECT_EQ(expected_ashr, Evaluate(aig, arithmetic_shift, values));
            EXPECT_EQ((left & 1u) != 0u ? left : right,
                      Evaluate(aig, selected, values));

            EXPECT_EQ(left == right ? 1u : 0u,
                      EvaluateBit(aig, equal, values));
            EXPECT_EQ(left < right ? 1u : 0u, EvaluateBit(aig, below, values));
            EXPECT_EQ(left <= right ? 1u : 0u,
                      EvaluateBit(aig, below_or_equal, values));
            EXPECT_EQ(signed_left < signed_right ? 1u : 0u,
                      EvaluateBit(aig, signed_below, values));
            EXPECT_EQ(signed_left <= signed_right ? 1u : 0u,
                      EvaluateBit(aig, signed_below_or_equal, values));
            EXPECT_EQ(left != 0u ? 1u : 0u, EvaluateBit(aig, any_bit, values));
            EXPECT_EQ(left == kMask ? 1u : 0u,
                      EvaluateBit(aig, all_bits, values));
        }
    }
}

TEST(AigBitVector, SignedDivisionOverflowWrapsLikeSmtLib) {
    constexpr std::uint32_t kWidth = 8u;
    Graph aig;
    ql_error error{};

    const Word a = MakeInput(aig, kWidth);
    const Word b = MakeInput(aig, kWidth);
    Word quotient(kWidth);
    Word remainder(kWidth);
    ASSERT_EQ(QL_STATUS_OK,
              ql_aig_bv_sdivrem(aig, a.data(), b.data(), kWidth,
                                quotient.data(), remainder.data(), &error));

    /* -128 / -1 has no eight-bit result. C leaves it undefined and SMT-LIB
       wraps it to -128. The circuit must produce the SMT-LIB value, because
       rejecting the operation is the UB guard's job and not the circuit's. */
    const std::vector<std::uint8_t> values = Assignment(0x80u, 0xffu, kWidth);
    EXPECT_EQ(0x80u, Evaluate(aig, quotient, values));
    EXPECT_EQ(0x00u, Evaluate(aig, remainder, values));
}

TEST(AigBitVector, WidthChangesKeepAndDropTheRightBits) {
    constexpr std::uint32_t kWidth = 4u;
    Graph aig;
    ql_error error{};

    const Word a = MakeInput(aig, kWidth);
    const Word b = MakeInput(aig, kWidth);
    Word zero_extended(8u);
    Word sign_extended(8u);
    Word truncated(2u);

    ASSERT_EQ(QL_STATUS_OK, ql_aig_bv_zext(aig, a.data(), kWidth, 8u,
                                           zero_extended.data(), &error));
    ASSERT_EQ(QL_STATUS_OK, ql_aig_bv_sext(aig, a.data(), kWidth, 8u,
                                           sign_extended.data(), &error));
    ASSERT_EQ(QL_STATUS_OK, ql_aig_bv_trunc(aig, a.data(), kWidth, 2u,
                                            truncated.data(), &error));
    EXPECT_EQ(QL_STATUS_INVALID_ARGUMENT,
              ql_aig_bv_zext(aig, a.data(), 8u, kWidth, zero_extended.data(),
                             &error));
    EXPECT_EQ(QL_STATUS_INVALID_ARGUMENT,
              ql_aig_bv_trunc(aig, a.data(), kWidth, 8u, truncated.data(),
                              &error));
    (void)b;

    for (std::uint32_t value = 0u; value < 16u; ++value) {
        const std::vector<std::uint8_t> values = Assignment(value, 0u, kWidth);
        EXPECT_EQ(value, Evaluate(aig, zero_extended, values));
        EXPECT_EQ(static_cast<std::uint32_t>(Signed(value, kWidth)) & 0xffu,
                  Evaluate(aig, sign_extended, values));
        EXPECT_EQ(value & 3u, Evaluate(aig, truncated, values));
    }
}

TEST(AigBitVector, ConstantsUseTheIrByteEncoding) {
    constexpr std::uint8_t kBytes[] = {0xa5u};
    constexpr std::uint8_t kNarrow[] = {0x05u};
    constexpr std::uint8_t kDirty[] = {0x0fu};
    Graph aig;
    Word word(8u);
    Word narrow(3u);
    ql_error error{};

    ASSERT_EQ(QL_STATUS_OK, ql_aig_bv_constant(aig, kBytes, sizeof(kBytes), 8u,
                                               word.data(), &error));
    for (std::uint32_t index = 0u; index < 8u; ++index) {
        EXPECT_EQ(((0xa5u >> index) & 1u) != 0u ? QL_AIG_LIT_TRUE
                                                : QL_AIG_LIT_FALSE,
                  word[index]);
    }
    ASSERT_EQ(QL_STATUS_OK, ql_aig_bv_constant(aig, kNarrow, sizeof(kNarrow),
                                               3u, narrow.data(), &error));
    /* An oversized byte count and a dirty high bit are both rejected rather
       than masked, because a silently masked constant is a wrong circuit. */
    EXPECT_EQ(QL_STATUS_INVALID_ARGUMENT,
              ql_aig_bv_constant(aig, kBytes, sizeof(kBytes), 3u,
                                 narrow.data(), &error));
    EXPECT_EQ(QL_STATUS_INVALID_ARGUMENT,
              ql_aig_bv_constant(aig, kDirty, sizeof(kDirty), 3u,
                                 narrow.data(), &error));
    EXPECT_EQ(QL_STATUS_INVALID_ARGUMENT,
              ql_aig_bv_constant(aig, kBytes, sizeof(kBytes),
                                 QL_AIG_MAX_BIT_WIDTH + 1u, word.data(),
                                 &error));
}

TEST(AigBitVector, EqualSubcircuitsOfTwoFunctionsCollapseIntoOneNode) {
    constexpr std::uint32_t kWidth = 8u;
    Graph aig;
    ql_aig_view_v1 before{};
    ql_aig_view_v1 after{};
    ql_error error{};

    const Word a = MakeInput(aig, kWidth);
    const Word b = MakeInput(aig, kWidth);
    Word left(kWidth);
    Word right(kWidth);

    ASSERT_EQ(QL_STATUS_OK, ql_aig_bv_add(aig, a.data(), b.data(),
                                          QL_AIG_LIT_FALSE, kWidth,
                                          left.data(), nullptr, &error));
    before.struct_size = sizeof(before);
    ASSERT_EQ(QL_STATUS_OK, ql_aig_get_view(aig, &before, &error));

    /* The commuted sum is the same circuit once the operand order is
       normalised, so a miter over two similar functions gets smaller before a
       solver ever sees it. */
    ASSERT_EQ(QL_STATUS_OK, ql_aig_bv_add(aig, b.data(), a.data(),
                                          QL_AIG_LIT_FALSE, kWidth,
                                          right.data(), nullptr, &error));
    after.struct_size = sizeof(after);
    ASSERT_EQ(QL_STATUS_OK, ql_aig_get_view(aig, &after, &error));
    EXPECT_EQ(before.and_count, after.and_count);
    EXPECT_EQ(left, right);
}

/* --- CNF ------------------------------------------------------------------ */

struct Formula {
    std::uint32_t variables = 0u;
    std::vector<std::vector<std::int32_t>> clauses;
};

Formula ParseDimacs(const std::string &text) {
    Formula formula;
    std::size_t cursor = 0u;
    std::vector<std::int32_t> current;

    while (cursor < text.size()) {
        const std::size_t end = text.find('\n', cursor);
        const std::string line =
            text.substr(cursor, end == std::string::npos ? std::string::npos
                                                         : end - cursor);
        cursor = end == std::string::npos ? text.size() : end + 1u;
        if (line.empty()) {
            continue;
        }
        if (line[0] == 'p') {
            const std::size_t header = line.find("cnf ");
            EXPECT_NE(std::string::npos, header);
            if (header == std::string::npos) {
                continue;
            }
            formula.variables = static_cast<std::uint32_t>(
                std::strtoul(line.c_str() + header + 4u, nullptr, 10));
            continue;
        }
        const char *position = line.c_str();
        while (*position != '\0') {
            char *next = nullptr;
            const long value = std::strtol(position, &next, 10);
            if (next == position) {
                break;
            }
            position = next;
            while (*position == ' ') {
                ++position;
            }
            if (value == 0) {
                formula.clauses.push_back(current);
                current.clear();
                continue;
            }
            current.push_back(static_cast<std::int32_t>(value));
        }
    }
    return formula;
}

bool Satisfies(const Formula &formula, std::uint32_t assignment) {
    for (const std::vector<std::int32_t> &clause : formula.clauses) {
        bool satisfied = false;
        for (const std::int32_t literal : clause) {
            const std::uint32_t variable =
                static_cast<std::uint32_t>(literal < 0 ? -literal : literal);
            const bool value =
                ((assignment >> (variable - 1u)) & 1u) != 0u;
            if (value == (literal > 0)) {
                satisfied = true;
                break;
            }
        }
        if (!satisfied) {
            return false;
        }
    }
    return true;
}

std::string ArtifactText(const ql_artifact *artifact) {
    ql_artifact_view view{};
    ql_error error{};
    view.struct_size = sizeof(view);
    EXPECT_EQ(QL_STATUS_OK, ql_artifact_get_view(artifact, &view, &error));
    return std::string(static_cast<const char *>(view.data), view.size);
}

/* The encoding is only worth anything if its models are exactly the circuit's
   satisfying inputs. Three bits per operand keeps the whole variable space
   small enough to enumerate, so this is a complete check rather than a
   sample. */
TEST(AigCnf, ModelsAreExactlyTheCircuitsSatisfyingInputs) {
    constexpr std::uint32_t kWidth = 2u;
    constexpr std::uint32_t kMask = (1u << kWidth) - 1u;
    Graph aig;
    ql_aig_cnf *cnf = nullptr;
    ql_aig_cnf_view_v1 view{};
    ql_artifact *artifact = nullptr;
    ql_error error{};

    const Word a = MakeInput(aig, kWidth);
    const Word b = MakeInput(aig, kWidth);
    Word sum(kWidth);
    ql_aig_lit root = QL_AIG_LIT_INVALID;
    Word target(kWidth);
    const std::uint8_t bytes[] = {0x01u};

    ASSERT_EQ(QL_STATUS_OK, ql_aig_bv_add(aig, a.data(), b.data(),
                                          QL_AIG_LIT_FALSE, kWidth,
                                          sum.data(), nullptr, &error));
    ASSERT_EQ(QL_STATUS_OK, ql_aig_bv_constant(aig, bytes, sizeof(bytes),
                                               kWidth, target.data(), &error));
    ASSERT_EQ(QL_STATUS_OK, ql_aig_bv_eq(aig, sum.data(), target.data(),
                                         kWidth, &root, &error));
    ASSERT_EQ(QL_STATUS_OK, ql_aig_cnf_create(nullptr, aig, root, &cnf,
                                              &error))
        << error.message;
    view.struct_size = sizeof(view);
    ASSERT_EQ(QL_STATUS_OK, ql_aig_cnf_get_view(cnf, &view, &error));
    EXPECT_EQ(0u, view.trivially_true);
    EXPECT_EQ(0u, view.trivially_false);
    ASSERT_LE(view.variable_count, 20u)
        << "the enumeration below assumes a small variable space";

    ASSERT_EQ(QL_STATUS_OK,
              ql_aig_cnf_artifact_create(nullptr, cnf, &artifact, &error));
    const Formula formula = ParseDimacs(ArtifactText(artifact));
    EXPECT_EQ(view.variable_count, formula.variables);
    EXPECT_EQ(view.clause_count, formula.clauses.size());

    std::vector<std::uint32_t> input_variables;
    for (std::uint32_t index = 0u; index < 2u * kWidth; ++index) {
        const std::uint32_t variable = ql_aig_cnf_input_variable(cnf, index);
        ASSERT_NE(0u, variable) << "input " << index << " is in the cone";
        input_variables.push_back(variable);
    }

    std::set<std::uint32_t> from_solver;
    const std::uint32_t space = 1u << formula.variables;
    for (std::uint32_t assignment = 0u; assignment < space; ++assignment) {
        if (!Satisfies(formula, assignment)) {
            continue;
        }
        std::uint32_t projected = 0u;
        for (std::size_t index = 0u; index < input_variables.size();
             ++index) {
            const std::uint32_t bit =
                (assignment >> (input_variables[index] - 1u)) & 1u;
            projected |= bit << index;
        }
        from_solver.insert(projected);
    }

    std::set<std::uint32_t> from_circuit;
    for (std::uint32_t left = 0u; left <= kMask; ++left) {
        for (std::uint32_t right = 0u; right <= kMask; ++right) {
            const std::vector<std::uint8_t> values =
                Assignment(left, right, kWidth);
            if (EvaluateBit(aig, root, values) != 0u) {
                from_circuit.insert(left | (right << kWidth));
            }
        }
    }
    EXPECT_FALSE(from_circuit.empty());
    EXPECT_EQ(from_circuit, from_solver);

    ql_artifact_release(artifact);
    ql_aig_cnf_destroy(cnf);
}

TEST(AigCnf, AFoldedRootNeedsNoSolver) {
    Graph aig;
    ql_aig_lit input = QL_AIG_LIT_INVALID;
    ql_aig_lit contradiction = QL_AIG_LIT_INVALID;
    ql_aig_cnf *trivial_true = nullptr;
    ql_aig_cnf *trivial_false = nullptr;
    ql_aig_cnf_view_v1 view{};
    ql_artifact *artifact = nullptr;
    ql_error error{};

    ASSERT_EQ(QL_STATUS_OK, ql_aig_add_input(aig, &input, &error));
    ASSERT_EQ(QL_STATUS_OK,
              ql_aig_and(aig, input, ql_aig_not(input), &contradiction,
                         &error));
    ASSERT_EQ(QL_AIG_LIT_FALSE, contradiction);

    ASSERT_EQ(QL_STATUS_OK, ql_aig_cnf_create(nullptr, aig, contradiction,
                                              &trivial_false, &error));
    view.struct_size = sizeof(view);
    ASSERT_EQ(QL_STATUS_OK, ql_aig_cnf_get_view(trivial_false, &view, &error));
    EXPECT_EQ(1u, view.trivially_false);
    EXPECT_EQ(0u, view.variable_count);
    EXPECT_EQ(1u, view.clause_count);
    ASSERT_EQ(QL_STATUS_OK, ql_aig_cnf_artifact_create(nullptr, trivial_false,
                                                       &artifact, &error));
    /* The empty clause. Nothing satisfies it, which is what a folded-false
       root means. */
    EXPECT_EQ("p cnf 0 1\n0\n", ArtifactText(artifact));
    ql_artifact_release(artifact);

    ASSERT_EQ(QL_STATUS_OK, ql_aig_cnf_create(nullptr, aig, QL_AIG_LIT_TRUE,
                                              &trivial_true, &error));
    ql_aig_cnf_view_v1 true_view{};
    true_view.struct_size = sizeof(true_view);
    ASSERT_EQ(QL_STATUS_OK,
              ql_aig_cnf_get_view(trivial_true, &true_view, &error));
    EXPECT_EQ(1u, true_view.trivially_true);
    EXPECT_EQ(0u, true_view.clause_count);

    ql_aig_cnf_destroy(trivial_true);
    ql_aig_cnf_destroy(trivial_false);
}

TEST(AigCnf, OnlyTheRootsConeGetsVariables) {
    Graph aig;
    ql_aig_cnf *cnf = nullptr;
    ql_aig_cnf_view_v1 view{};
    ql_error error{};

    const Word a = MakeInput(aig, 4u);
    const Word b = MakeInput(aig, 4u);
    Word unused(4u);
    ql_aig_lit root = QL_AIG_LIT_INVALID;

    /* A wide product nobody asks about must cost the solver nothing. */
    ASSERT_EQ(QL_STATUS_OK, ql_aig_bv_mul(aig, a.data(), b.data(), 4u,
                                          unused.data(), &error));
    ASSERT_EQ(QL_STATUS_OK,
              ql_aig_and(aig, a[0], a[1], &root, &error));
    ASSERT_EQ(QL_STATUS_OK, ql_aig_cnf_create(nullptr, aig, root, &cnf,
                                              &error));
    view.struct_size = sizeof(view);
    ASSERT_EQ(QL_STATUS_OK, ql_aig_cnf_get_view(cnf, &view, &error));
    /* Two inputs and the one AND node, and nothing else. */
    EXPECT_EQ(3u, view.variable_count);
    EXPECT_NE(0u, ql_aig_cnf_input_variable(cnf, 0u));
    EXPECT_NE(0u, ql_aig_cnf_input_variable(cnf, 1u));
    EXPECT_EQ(0u, ql_aig_cnf_input_variable(cnf, 2u));
    EXPECT_EQ(0u, ql_aig_cnf_input_variable(cnf, 7u));
    ql_aig_cnf_destroy(cnf);
}

TEST(AigCnf, TheSameCircuitAlwaysSerializesToTheSameBytes) {
    ql_error error{};
    std::string first;
    std::string second;

    for (int round = 0; round < 2; ++round) {
        Graph aig;
        const Word a = MakeInput(aig, 5u);
        const Word b = MakeInput(aig, 5u);
        Word product(5u);
        ql_aig_lit root = QL_AIG_LIT_INVALID;
        ql_aig_cnf *cnf = nullptr;
        ql_artifact *artifact = nullptr;

        ASSERT_EQ(QL_STATUS_OK, ql_aig_bv_mul(aig, a.data(), b.data(), 5u,
                                              product.data(), &error));
        ASSERT_EQ(QL_STATUS_OK, ql_aig_bv_reduce_or(aig, product.data(), 5u,
                                                    &root, &error));
        ASSERT_EQ(QL_STATUS_OK,
                  ql_aig_cnf_create(nullptr, aig, root, &cnf, &error));
        ASSERT_EQ(QL_STATUS_OK,
                  ql_aig_cnf_artifact_create(nullptr, cnf, &artifact, &error));
        (round == 0 ? first : second) = ArtifactText(artifact);
        ql_artifact_release(artifact);
        ql_aig_cnf_destroy(cnf);
    }
    EXPECT_EQ(first, second);
    EXPECT_EQ(0u, first.compare(0u, 6u, "p cnf "));
}

}  // namespace
