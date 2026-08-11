"""The follow-up chain: bounded refutation, CHC/PDR proof, and its gates.

The chain enters only where the SMT product failed to answer -- the loop
territory -- and never where an answer exists behind a trust policy. Its two
adoptions are the two grounded directions: a counterexample replayed on the
original cyclic functions, and an inductive proof promoted under the same
trusted-backend policy as the product's own UNSAT.
"""

from __future__ import annotations

import quodlibet

#: The pair the fast path cannot close: the invariant i + j = n names an
#: input, which is outside its constant-coefficient relation vocabulary, and
#: CHC/PDR's entry-anchored templates close it.
COUNT_UP = (
    "unsigned up(unsigned n) { unsigned i = 0u; unsigned s = 0u;"
    " while (i != n) { s = s + 2u; i = i + 1u; } return s; }"
)
COUNT_DOWN = (
    "unsigned down(unsigned n) { unsigned j = n; unsigned t = 0u;"
    " while (j != 0u) { t = t + 2u; j = j - 1u; } return t; }"
)
#: Same shape, different step: differs at n >= 1, well inside a small bound.
COUNT_DOWN_WRONG = (
    "unsigned down(unsigned n) { unsigned j = n; unsigned t = 0u;"
    " while (j != 0u) { t = t + 3u; j = j - 1u; } return t; }"
)


def test_chc_pdr_closes_the_pair_the_fast_path_cannot(backend):
    bare = quodlibet.check(
        COUNT_UP,
        "up",
        COUNT_DOWN,
        "down",
        observations=["return-value"],
        trust_smt_backend=True,
        bounded_unroll=0,
        chc_pdr=False,
    )
    assert bare.verdict == "unknown"

    result = quodlibet.check(
        COUNT_UP,
        "up",
        COUNT_DOWN,
        "down",
        observations=["return-value"],
        trust_smt_backend=True,
    )
    assert result.verdict == "proved-equivalent"
    assert result.decided_by == "prove.chc-pdr"
    # The promotion rests on the trusted backend, and the envelope says so.
    assert result.evidence.checked_proof is False
    assert result.evidence.unsat_promotion == "trusted-backend"


def test_a_chc_pdr_fixpoint_is_not_promoted_without_the_trust_policy(backend):
    result = quodlibet.check(
        COUNT_UP,
        "up",
        COUNT_DOWN,
        "down",
        observations=["return-value"],
        trust_smt_backend=False,
        bounded_unroll=0,
    )
    assert result.verdict == "unknown"
    assert result.decided_by == "prove.smt-product"


def test_bounded_refutes_a_loop_pair_and_replays_it(backend):
    result = quodlibet.check(
        COUNT_UP,
        "up",
        COUNT_DOWN_WRONG,
        "down",
        observations=["return-value"],
        trust_smt_backend=True,
    )
    assert result.verdict == "counterexample"
    assert result.decided_by == "search.bounded-symbolic"
    # Only a model replayed on the original cyclic functions is reported.
    assert result.evidence.replay_confirmed is True
    assert result.counterexample is not None


def test_a_clean_bound_is_bounded_clean_and_never_a_proof(backend):
    """Unpaired loop shapes: CHC/PDR has no prefix, the bound finds no
    difference, and the strongest honest claim is bounded-clean."""
    looping = (
        "int add(int x, int y){ while (y) { x = x + 1; y = y - 1; }"
        " return x; }"
    )
    summing = "int sum(int a, int b){ return b + a; }"
    result = quodlibet.check(
        looping, "add", summing, "sum", trust_smt_backend=True
    )
    assert result.verdict == "bounded-clean"
    assert result.decided_by == "search.bounded-symbolic"
    assert result.evidence.checked_proof is False
    assert result.proved is False


def test_the_chain_stays_out_of_the_trust_policy_territory(
    backend, equivalent_pair
):
    """A violation UNSAT withheld by the trust policy is stronger evidence
    than anything the refuter could add; the chain must not run there and
    must not weaken `unknown` into `bounded-clean`."""
    result = quodlibet.check(**equivalent_pair)
    assert result.verdict == "unknown"
    assert result.decided_by == "prove.smt-product"
    assert result.evidence.violation_answer == "unsat"
