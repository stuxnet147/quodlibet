#include "quodlibet/proof_aigsat.h"

#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "quodlibet/product.h"
#include "quodlibet/replay.h"
#include "quodlibet/solver.h"
#include "fuzz/fuzz_blaster_target.h"
#include "w2_fixtures.h"

namespace {

class Graph {
public:
    Graph() {
        ql_error error{};
        EXPECT_EQ(QL_STATUS_OK, ql_aig_create(nullptr, &aig_, &error));
    }
    Graph(const Graph &) = delete;
    Graph &operator=(const Graph &) = delete;
    ~Graph() { ql_aig_destroy(aig_); }
    operator ql_aig *() const { return aig_; }
    ql_aig *get() const { return aig_; }

private:
    ql_aig *aig_ = nullptr;
};

class Blast {
public:
    Blast() = default;
    Blast(const Blast &) = delete;
    Blast &operator=(const Blast &) = delete;
    ~Blast() { ql_aig_blast_destroy(blast_); }

    ql_status Run(ql_aig *aig, const std::vector<const ql_artifact *> &parts,
                  ql_error *error) {
        return ql_aig_blast_smt2(aig, parts.data(), parts.size(), &blast_,
                                 error);
    }
    ql_aig_blast *get() const { return blast_; }

    ql_aig_blast_view_v1 view() const {
        ql_aig_blast_view_v1 result{};
        ql_error error{};
        result.struct_size = sizeof(result);
        EXPECT_EQ(QL_STATUS_OK,
                  ql_aig_blast_get_view(blast_, &result, &error))
            << error.message;
        return result;
    }

private:
    ql_aig_blast *blast_ = nullptr;
};

/* A one-artifact query written by the same builder the product encoder uses,
   so a grammar test states only the text it cares about. */
class Query {
public:
    Query() {
        ql_error error{};
        EXPECT_EQ(QL_STATUS_OK,
                  ql_smt2_builder_create(nullptr, QL_SOLVER_LOGIC_QF_BV,
                                         &builder_, &error))
            << error.message;
    }
    Query(const Query &) = delete;
    Query &operator=(const Query &) = delete;
    ~Query() {
        ql_artifact_release(artifact_);
        ql_smt2_builder_destroy(builder_);
    }

    ql_smt2_builder *builder() const { return builder_; }

    const ql_artifact *Build() {
        ql_error error{};
        EXPECT_EQ(QL_STATUS_OK,
                  ql_smt2_builder_build(builder_, &artifact_, &error))
            << error.message;
        return artifact_;
    }

private:
    ql_smt2_builder *builder_ = nullptr;
    ql_artifact *artifact_ = nullptr;
};

/* A raw text artifact, for the inputs the builder would refuse to write but a
   hostile or drifting producer might hand over anyway. */
ql_artifact *RawQuery(const std::string &text) {
    ql_artifact *artifact = nullptr;
    ql_error error{};
    EXPECT_EQ(QL_STATUS_OK,
              ql_artifact_create(nullptr, QL_ARTIFACT_KIND_SMTLIB2, 1u,
                                 text.data(), text.size(), &artifact,
                                 &error));
    return artifact;
}

std::uint32_t EvaluateRoot(ql_aig *aig, ql_aig_lit root,
                           const std::vector<std::uint8_t> &values) {
    std::uint32_t bit = 0u;
    ql_error error{};
    EXPECT_EQ(QL_STATUS_OK,
              ql_aig_evaluate(aig, values.data(), values.size(), root, &bit,
                              &error))
        << error.message;
    return bit;
}

/* --- Grammar -------------------------------------------------------------- */

TEST(AigBlast, BlastsTheSortsAndOperatorsTheBuilderEmits) {
    Query query;
    Graph aig;
    Blast blast;
    ql_error error{};

    ASSERT_EQ(QL_STATUS_OK,
              ql_smt2_builder_declare_bv(query.builder(), "x", 8u, &error));
    ASSERT_EQ(QL_STATUS_OK,
              ql_smt2_builder_declare_bv(query.builder(), "y", 8u, &error));
    ASSERT_EQ(QL_STATUS_OK,
              ql_smt2_builder_declare_bool(query.builder(), "p", &error));
    ASSERT_EQ(QL_STATUS_OK,
              ql_smt2_builder_define_bv(query.builder(), "s", 8u,
                                        "(bvadd x y)", &error));
    ASSERT_EQ(QL_STATUS_OK,
              ql_smt2_builder_define_bv(query.builder(), "w", 16u,
                                        "((_ zero_extend 8) s)", &error));
    ASSERT_EQ(QL_STATUS_OK,
              ql_smt2_builder_define_bv(query.builder(), "t", 8u,
                                        "((_ extract 7 0) w)", &error));
    ASSERT_EQ(QL_STATUS_OK,
              ql_smt2_builder_define_bool(query.builder(), "q",
                                          "(and true (= s t)"
                                          " (or (bvult x y) (bvsge y x))"
                                          " (=> p (distinct x (_ bv0 8)))"
                                          " (not (bvsle (bvneg x) (bvnot y))))",
                                          &error));
    ASSERT_EQ(QL_STATUS_OK,
              ql_smt2_builder_define_bv(query.builder(), "u", 8u,
                                        "(ite p (bvsdiv x y) (bvurem x y))",
                                        &error));
    ASSERT_EQ(QL_STATUS_OK,
              ql_smt2_builder_assert(query.builder(), "(= u #b00000001)",
                                     &error));

    ASSERT_EQ(QL_STATUS_OK, blast.Run(aig, {query.Build()}, &error))
        << error.message;
    const ql_aig_blast_view_v1 view = blast.view();
    EXPECT_EQ(3u, view.declared_count);
    EXPECT_EQ(5u, view.defined_count);
    EXPECT_EQ(1u, view.assertion_count);
    EXPECT_EQ(3u, ql_aig_blast_symbol_count(blast.get()));

    ql_aig_blast_symbol_v1 symbol{};
    symbol.struct_size = sizeof(symbol);
    ASSERT_EQ(QL_STATUS_OK,
              ql_aig_blast_symbol_by_name(blast.get(), "x", &symbol, &error));
    EXPECT_EQ(0u, symbol.is_bool);
    EXPECT_EQ(8u, symbol.bit_width);
    EXPECT_EQ(0u, symbol.first_input);
    ASSERT_EQ(QL_STATUS_OK,
              ql_aig_blast_symbol_by_name(blast.get(), "p", &symbol, &error));
    EXPECT_EQ(1u, symbol.is_bool);
    EXPECT_EQ(16u, symbol.first_input);
    /* A defined symbol is a circuit, not a free variable, so a model assigns
       it nothing and it is not offered for decoding. */
    EXPECT_EQ(QL_STATUS_NOT_FOUND,
              ql_aig_blast_symbol_by_name(blast.get(), "s", &symbol, &error));

    /* The asserted root must be exactly the predicate it spells: p selects
       signed division, and the assertion pins its result to one. */
    for (std::uint32_t x = 0u; x < 256u; x += 37u) {
        for (std::uint32_t y = 0u; y < 256u; y += 41u) {
            for (std::uint32_t p = 0u; p < 2u; ++p) {
                std::vector<std::uint8_t> values;
                for (std::uint32_t bit = 0u; bit < 8u; ++bit) {
                    values.push_back(
                        static_cast<std::uint8_t>((x >> bit) & 1u));
                }
                for (std::uint32_t bit = 0u; bit < 8u; ++bit) {
                    values.push_back(
                        static_cast<std::uint8_t>((y >> bit) & 1u));
                }
                values.push_back(static_cast<std::uint8_t>(p));

                const std::int32_t sx =
                    (x & 0x80u) != 0u ? static_cast<std::int32_t>(x) - 256
                                      : static_cast<std::int32_t>(x);
                const std::int32_t sy =
                    (y & 0x80u) != 0u ? static_cast<std::int32_t>(y) - 256
                                      : static_cast<std::int32_t>(y);
                std::uint32_t expected_u;
                if (p != 0u) {
                    expected_u = sy == 0 ? (sx >= 0 ? 0xffu : 1u)
                                         : static_cast<std::uint32_t>(
                                               sx / sy) & 0xffu;
                } else {
                    expected_u = y == 0u ? x : x % y;
                }
                EXPECT_EQ(expected_u == 1u ? 1u : 0u,
                          EvaluateRoot(aig, view.root, values))
                    << "x=" << x << " y=" << y << " p=" << p;
            }
        }
    }
}

TEST(AigBlast, RefusesEverythingOutsideTheEmittedGrammar) {
    struct Case {
        const char *text;
        ql_status status;
        const char *fragment;
    };
    /* The blaster is a front end for exactly one producer. Anything else is a
       refusal that becomes UNKNOWN, never a guess. */
    const Case cases[] = {
        {"(set-logic QF_BV)\n(declare-const m (Array (_ BitVec 8) (_ BitVec 8)))\n",
         QL_STATUS_TYPE_MISMATCH, "Array"},
        {"(set-logic QF_BV)\n(push 1)\n", QL_STATUS_TYPE_MISMATCH,
         "top-level command"},
        {"(set-logic QF_BV)\n(check-sat)\n", QL_STATUS_TYPE_MISMATCH,
         "top-level command"},
        {"(set-logic QF_BV)\n(assert (bvcomp x y))\n", QL_STATUS_TYPE_MISMATCH,
         "operator outside"},
        {"(set-logic QF_BV)\n(assert (= x x))\n", QL_STATUS_NOT_FOUND,
         "undeclared symbol"},
        {"(set-logic QF_BV)\n(declare-const x Bool)\n(declare-const x Bool)\n",
         QL_STATUS_ALREADY_EXISTS, "twice"},
        {"(set-logic QF_BV)\n(define-fun f ((a Bool)) Bool a)\n",
         QL_STATUS_TYPE_MISMATCH, "parameters"},
        {"(set-logic QF_BV)\n(declare-const x (_ BitVec 8))\n(assert x)\n",
         QL_STATUS_TYPE_MISMATCH, "Boolean was required"},
        {"(set-logic QF_BV)\n(declare-const b Bool)\n"
         "(declare-const x (_ BitVec 8))\n(assert (= b x))\n",
         QL_STATUS_TYPE_MISMATCH, "two different sorts"},
        {"(set-logic QF_BV)\n(declare-const x (_ BitVec 8))\n"
         "(declare-const y (_ BitVec 4))\n(assert (= x y))\n",
         QL_STATUS_TYPE_MISMATCH, "two different sorts"},
        {"(set-logic QF_BV)\n(declare-const x (_ BitVec 8))\n"
         "(assert (= ((_ extract 7 1) x) ((_ extract 7 1) x)))\n",
         QL_STATUS_TYPE_MISMATCH, "low index is not zero"},
        {"(set-logic QF_BV)\n(declare-const x (_ BitVec 4096))\n",
         QL_STATUS_TYPE_MISMATCH, "wider than"},
    };

    for (const Case &current : cases) {
        Graph aig;
        Blast blast;
        ql_error error{};
        ql_artifact *artifact = RawQuery(current.text);
        ASSERT_NE(nullptr, artifact);
        EXPECT_EQ(current.status, blast.Run(aig, {artifact}, &error))
            << current.text;
        EXPECT_NE(nullptr, std::strstr(error.message, current.fragment))
            << current.text << " gave: " << error.message;
        ql_artifact_release(artifact);
    }
}

TEST(AigBlast, RefusesAnArtifactThatIsNotSmtLib) {
    Graph aig;
    Blast blast;
    ql_artifact *artifact = nullptr;
    ql_error error{};

    ASSERT_EQ(QL_STATUS_OK,
              ql_artifact_create(nullptr, QL_ARTIFACT_KIND_PROBLEM, 1u, "x",
                                 1u, &artifact, &error));
    EXPECT_EQ(QL_STATUS_TYPE_MISMATCH, blast.Run(aig, {artifact}, &error));
    ql_artifact_release(artifact);
}

TEST(AigBlast, AnEmptyQueryAssertsNothing) {
    Query query;
    Graph aig;
    Blast blast;
    ql_error error{};

    ASSERT_EQ(QL_STATUS_OK, blast.Run(aig, {query.Build()}, &error));
    /* Nothing asserted is satisfiable, and the root says so without a
       solver. */
    EXPECT_EQ(QL_AIG_LIT_TRUE, blast.view().root);
    EXPECT_EQ(0u, blast.view().assertion_count);
}

TEST(AigBlast, PartsBlastAsOneTextInOrder) {
    Query prefix;
    Query terminal;
    Graph aig;
    Blast blast;
    ql_error error{};

    ASSERT_EQ(QL_STATUS_OK,
              ql_smt2_builder_declare_bv(prefix.builder(), "x", 4u, &error));
    ASSERT_EQ(QL_STATUS_OK,
              ql_smt2_builder_define_bool(prefix.builder(), "big",
                                          "(bvugt x #b0111)", &error));
    /* The terminal part references a symbol the prefix defined, exactly as the
       solver would read the two concatenated. */
    ASSERT_EQ(QL_STATUS_OK,
              ql_smt2_builder_assert(terminal.builder(), "big", &error));

    ASSERT_EQ(QL_STATUS_OK,
              blast.Run(aig, {prefix.Build(), terminal.Build()}, &error))
        << error.message;
    const ql_aig_blast_view_v1 view = blast.view();
    for (std::uint32_t value = 0u; value < 16u; ++value) {
        std::vector<std::uint8_t> values;
        for (std::uint32_t bit = 0u; bit < 4u; ++bit) {
            values.push_back(static_cast<std::uint8_t>((value >> bit) & 1u));
        }
        EXPECT_EQ(value > 7u ? 1u : 0u, EvaluateRoot(aig, view.root, values));
    }
}

/* --- Agreement with the interpreter --------------------------------------- */

/* This is the test the whole architecture rests on. The AIG path claims a
   checked proof about the same query the SMT path sends to Bitwuzla, so the
   blasted miter must call an input a violation exactly when concretely running
   both functions does. A disagreement is a bug in the blaster or in the
   encoder, and it must fail loudly rather than be averaged away. */
class AgreementFixture {
public:
    void Check(const char *left_source, const char *left_name,
               const char *right_source, const char *right_name,
               const ql_semantic_contract_v1 &contract) {
        w2::Pair pair;
        Graph aig;
        Blast blast;
        ql_product_query *query = nullptr;
        ql_product_query_view_v1 query_view{};
        ql_error error{};

        ASSERT_EQ(QL_STATUS_OK, pair.Build(left_source, left_name,
                                           right_source, right_name, contract,
                                           &error))
            << error.message;
        ASSERT_EQ(QL_STATUS_OK,
                  ql_product_query_build(nullptr, pair.problem(),
                                         pair.left_ir(), pair.right_ir(),
                                         &query, &error))
            << error.message;
        query_view.struct_size = sizeof(query_view);
        ASSERT_EQ(QL_STATUS_OK,
                  ql_product_query_get_view(query, &query_view, &error));

        ASSERT_EQ(QL_STATUS_OK,
                  blast.Run(aig,
                            {ql_product_query_prefix_artifact(query),
                             ql_product_query_violation_artifact(query)},
                            &error))
            << error.message;
        const ql_aig_blast_view_v1 blast_view = blast.view();
        /* Every free variable of the miter must be a declared product input.
           If the query declared anything else, the circuit's answer would not
           be a function of the inputs alone and this comparison would be
           meaningless. */
        ASSERT_EQ(query_view.input_count, blast_view.declared_count);

        for (std::uint32_t round = 0u; round < kRounds; ++round) {
            std::vector<std::uint8_t> aig_values;
            std::string model = "(";
            bool skip = false;

            for (std::size_t index = 0u; index < query_view.input_count;
                 ++index) {
                ql_product_input_v1 input{};
                ql_aig_blast_symbol_v1 symbol{};
                symbol.struct_size = sizeof(symbol);
                ASSERT_EQ(QL_STATUS_OK, ql_product_query_input_at(
                                            query, index, &input, &error));
                ASSERT_EQ(QL_STATUS_OK,
                          ql_aig_blast_symbol_by_name(blast.get(),
                                                      input.symbol, &symbol,
                                                      &error))
                    << input.symbol;
                const std::uint32_t width =
                    input.kind == QL_SOURCE_TYPE_BOOL ? 1u : input.bit_width;
                if (width > 64u) {
                    skip = true;
                    break;
                }
                ASSERT_EQ(aig_values.size(), symbol.first_input)
                    << "declaration order must match the input order";

                const std::uint64_t value = Sample(round, index, width);
                std::string bits;
                for (std::uint32_t bit = width; bit != 0u; --bit) {
                    bits.push_back(((value >> (bit - 1u)) & 1u) != 0u ? '1'
                                                                      : '0');
                }
                for (std::uint32_t bit = 0u; bit < width; ++bit) {
                    aig_values.push_back(static_cast<std::uint8_t>(
                        (value >> bit) & 1u));
                }
                model += "(define-fun ";
                model += input.symbol;
                if (input.kind == QL_SOURCE_TYPE_BOOL) {
                    model += " () Bool ";
                    model += bits == "1" ? "true" : "false";
                } else {
                    model += " () (_ BitVec " + std::to_string(width) +
                             ") #b" + bits;
                }
                model += ")";
            }
            if (skip) {
                continue;
            }

            ql_replay_witness *witness = nullptr;
            ql_artifact *model_artifact = nullptr;
            model += ")";
            ASSERT_EQ(QL_STATUS_OK,
                      ql_artifact_create(nullptr,
                                         QL_ARTIFACT_KIND_SOLVER_MODEL, 1u,
                                         model.data(), model.size(),
                                         &model_artifact, &error));
            ASSERT_EQ(QL_STATUS_OK,
                      ql_replay_decode_model(nullptr, query, model_artifact,
                                             &witness, &error))
                << error.message;
            ql_artifact_release(model_artifact);

            ql_replay_result_v1 replay{};
            replay.struct_size = sizeof(replay);
            ASSERT_EQ(QL_STATUS_OK,
                      ql_replay_execute(nullptr, pair.problem(), query,
                                        pair.left_ir(), pair.right_ir(),
                                        witness, &replay, &error));
            ql_replay_witness_destroy(witness);

            const std::uint32_t circuit =
                EvaluateRoot(aig, blast_view.root, aig_values);
            if (replay.conclusive != 0u) {
                EXPECT_EQ(replay.violated, circuit)
                    << "round " << round
                    << ": the blasted miter and the interpreter disagree";
            } else if (replay.precondition_evaluated != 0u &&
                       replay.precondition_holds == 0u) {
                /* Outside the compared domain the miter must not call the
                   input a violation. */
                EXPECT_EQ(0u, circuit) << "round " << round;
            }
        }
        ql_product_query_destroy(query);
    }

private:
    static constexpr std::uint32_t kRounds = 96u;

    /* Boundary values first, then a deterministic spread. */
    static std::uint64_t Sample(std::uint32_t round, std::size_t index,
                                std::uint32_t width) {
        static const std::uint64_t kBoundary[] = {0u, 1u, 2u, 3u, 7u, 8u,
                                                  0x7fffffffu, 0x80000000u,
                                                  0xffffffffu, 0xfffffffeu};
        const std::uint64_t mask =
            width >= 64u ? ~UINT64_C(0) : ((UINT64_C(1) << width) - 1u);
        if (round < sizeof(kBoundary) / sizeof(kBoundary[0])) {
            return kBoundary[round] & mask;
        }
        std::uint64_t state = (static_cast<std::uint64_t>(round) << 32) +
                              static_cast<std::uint64_t>(index) +
                              UINT64_C(0x9e3779b97f4a7c15);
        state ^= state >> 30;
        state *= UINT64_C(0xbf58476d1ce4e5b9);
        state ^= state >> 27;
        state *= UINT64_C(0x94d049bb133111eb);
        state ^= state >> 31;
        return state & mask;
    }
};

TEST(AigBlast, TheMiterAgreesWithTheInterpreterOnEquivalentFunctions) {
    AgreementFixture fixture;
    fixture.Check("int add(int x, int y){ return x + y; }", "add",
                  "int sum(int a, int b){ return b + a; }", "sum",
                  w2::DefaultContract());
}

TEST(AigBlast, TheMiterAgreesWithTheInterpreterOnADifferingPair) {
    AgreementFixture fixture;
    fixture.Check("int f(int x){ return x; }", "f",
                  "int g(int x){ if (x == 7) return 7; return 0; }", "g",
                  w2::ContractObserving(QL_OBSERVE_RETURN_VALUE));
}

TEST(AigBlast, TheMiterAgreesWithTheInterpreterAcrossPartialOperations) {
    AgreementFixture fixture;
    ql_semantic_contract_v1 contract =
        w2::ContractObserving(QL_OBSERVE_RETURN_VALUE);
    contract.ub_policy = QL_UB_LANGUAGE_REFINEMENT;
    /* Division, remainder, and shifts are where a totalized circuit and a
       guard-aware interpreter are most likely to drift apart. */
    fixture.Check(
        "int f(int x, int y){ return (x / y) + (x % y) + (x << (y & 31)); }",
        "f",
        "int g(int a, int b){ if (b == 0) return 0;"
        " return (a % b) + (a / b) + (a << (b & 31)); }",
        "g", contract);
}

TEST(AigBlast, TheMiterAgreesWithTheInterpreterUnderAPrecondition) {
    constexpr char precondition[] =
        "{\"schema_version\":1,\"expression\":"
        "{\"op\":\"sgt\",\"left\":{\"op\":\"arg\",\"index\":0},"
        "\"right\":{\"op\":\"int\",\"signed\":true,\"width\":32,"
        "\"value\":\"0\"}}}";
    AgreementFixture fixture;
    ql_semantic_contract_v1 contract =
        w2::ContractObserving(QL_OBSERVE_RETURN_VALUE);

    contract.precondition_json = precondition;
    contract.precondition_json_size = sizeof(precondition) - 1u;
    fixture.Check("int f(int x){ return x; }", "f",
                  "int g(int x){ if (x < 0) return 0; return x; }", "g",
                  contract);
}

/* --- Fuzzing -------------------------------------------------------------- */

/* The blaster is the only part of the AIG path that reads bytes it did not
   write, so it gets a fuzz target. The coverage-guided campaign runs from
   tests/fuzz/fuzz_smt2_blaster.c; this runs the same target body on a
   deterministic mutation corpus, so the property is checked on every ctest
   and not only when someone remembers to build the fuzzers. */
TEST(AigBlastFuzz, MutatedQueriesNeverCrashAndAlwaysRefuseOrBlastCleanly) {
    static const char *const kSeeds[] = {
        "(set-logic QF_BV)\n(declare-const x (_ BitVec 8))\n"
        "(define-fun s () (_ BitVec 8) (bvadd x #b00000001))\n"
        "(assert (bvult s x))\n",
        "(set-logic QF_BV)\n(declare-const p Bool)\n"
        "(declare-const y (_ BitVec 16))\n"
        "(define-fun t () (_ BitVec 16) (ite p (bvsdiv y y) (bvneg y)))\n"
        "(assert (and true (= t y) (or p (bvsge y y))))\n",
        "(set-logic QF_BV)\n(declare-const z (_ BitVec 4))\n"
        "(define-fun w () (_ BitVec 8) ((_ sign_extend 4) z))\n"
        "(define-fun v () (_ BitVec 4) ((_ extract 3 0) w))\n"
        "(assert (distinct v z))\n",
    };
    std::uint64_t state = 0x243f6a8885a308d3ull;

    for (const char *seed : kSeeds) {
        const std::string base(seed);
        /* Byte flips, truncations, and splices: enough to walk the parser off
           its grammar in every direction without a coverage-guided engine. */
        for (int round = 0; round < 400; ++round) {
            std::string mutated = base;
            state ^= state >> 30;
            state *= 0xbf58476d1ce4e5b9ull;
            state ^= state >> 27;
            state *= 0x94d049bb133111ebull;
            state ^= state >> 31;

            const std::size_t choice = static_cast<std::size_t>(state % 3u);
            const std::size_t position =
                static_cast<std::size_t>((state >> 8) % mutated.size());
            if (choice == 0u) {
                mutated[position] =
                    static_cast<char>((state >> 24) & 0x7fu);
            } else if (choice == 1u) {
                mutated.resize(position);
            } else {
                mutated.insert(position,
                               std::string(1u + (state >> 40) % 4u, '('));
            }
            ql_fuzz_smt2_blaster(
                reinterpret_cast<const unsigned char *>(mutated.data()),
                mutated.size());
        }
        /* The unmutated seed must still blast, or the corpus is testing
           nothing. */
        ql_fuzz_smt2_blaster(
            reinterpret_cast<const unsigned char *>(base.data()),
            base.size());
    }
}

}  // namespace
