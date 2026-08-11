"""The three end-to-end verdicts, and the boundaries that must not move."""

from __future__ import annotations

import pytest

import quodlibet

from conftest import (
    ADD,
    DIVIDE,
    GUARDED_DIVIDE,
    IDENTITY,
    LOOPING,
    SEVEN,
    SUM,
)


def test_proof_requires_the_recorded_trust_policy(backend, equivalent_pair):
    """Bitwuzla exposes no proof object, so an UNSAT is only evidence until
    the caller opts into the recorded trusted-backend policy."""
    without = quodlibet.check(**equivalent_pair)
    assert without.verdict == "unknown"
    assert without.evidence.violation_answer == "unsat"
    assert without.evidence.domain_answer == "sat"
    assert without.evidence.unsat_promotion == "none"
    assert "evidence only" in without.diagnostic

    with_trust = quodlibet.check(trust_smt_backend=True, **equivalent_pair)
    assert with_trust.verdict == "proved-equivalent"
    assert with_trust.proved
    assert with_trust.evidence.unsat_promotion == "trusted-backend"
    # The envelope must keep saying no certificate was checked.
    assert with_trust.evidence.checked_proof is False
    assert with_trust.evidence.solver_binary_digest != ""
    assert with_trust.evidence.cache_key != without.evidence.cache_key


def test_counterexample_is_replayed_before_it_is_reported(backend):
    result = quodlibet.check(
        IDENTITY,
        "f",
        SEVEN,
        "g",
        observations=["return-value"],
        trust_smt_backend=True,
    )
    assert result.verdict == "counterexample"
    assert result.evidence.replay_confirmed is True
    assert "concrete replay" in result.diagnostic
    assert result.counterexample is not None
    assert result.counterexample["replayed"] is True
    assert result.counterexample["inputs"][0]["value"] == "ffffffff"
    assert result.evidence.counterexample_digest != ""


def test_a_cyclic_source_stays_unknown_for_the_loop_free_method(backend):
    result = quodlibet.check(LOOPING, "add", SUM, "sum")
    assert result.verdict == "unknown"
    assert result.status.kind == "method"
    assert result.status.ok is True
    assert "loop-free" in result.diagnostic
    assert result.counterexample is None


def test_a_source_outside_the_lowering_slice_stays_unknown(backend):
    atomic = (
        "int add(int x, int y){ _Atomic int observed = x; "
        "return observed + y; }"
    )
    result = quodlibet.check(atomic, "add", SUM, "sum")
    assert result.verdict == "unknown"
    assert result.status.kind == "unsupported"
    assert result.status.ok is False
    assert "restricted-C slice" in result.diagnostic
    assert result.counterexample is None


def test_refinement_directions_are_discharged_separately(backend):
    common = {
        "left_source": DIVIDE,
        "left_function": "f",
        "right_source": GUARDED_DIVIDE,
        "right_function": "g",
        "observations": ["return-value"],
        "ub_policy": "language-refinement",
        "trust_smt_backend": True,
    }
    right = quodlibet.check(relation="right-refines-left", **common)
    assert right.verdict == "proved-right-refines-left"

    left = quodlibet.check(relation="left-refines-right", **common)
    assert left.verdict == "counterexample"
    assert left.evidence.replay_confirmed is True


def test_a_vacuous_domain_never_becomes_a_proof(backend):
    precondition = {
        "schema_version": 1,
        "expression": {
            "op": "and",
            "args": [
                {
                    "op": "slt",
                    "left": {"op": "arg", "index": 0},
                    "right": {
                        "op": "int",
                        "signed": True,
                        "width": 32,
                        "value": "0",
                    },
                },
                {
                    "op": "sgt",
                    "left": {"op": "arg", "index": 0},
                    "right": {
                        "op": "int",
                        "signed": True,
                        "width": 32,
                        "value": "0",
                    },
                },
            ],
        },
    }
    result = quodlibet.check(
        IDENTITY,
        "f",
        "int g(int x){ return 0; }",
        "g",
        precondition_json=precondition,
        trust_smt_backend=True,
    )
    assert result.evidence.violation_answer == "unsat"
    assert result.evidence.domain_answer == "unsat"
    assert result.verdict == "unknown"
    assert "vacuous" in result.diagnostic


def test_the_solver_executable_can_be_named_explicitly(backend, tmp_path):
    """A wheel and the solver can be deployed apart, so the path the core was
    compiled with must be overridable per call."""
    result = quodlibet.check(
        ADD,
        "add",
        SUM,
        "sum",
        trust_smt_backend=True,
        solver_executable=backend["executable"],
    )
    assert result.verdict == "proved-equivalent"

    # A wrong path is an error, never a quietly weaker verdict.
    missing = tmp_path / "not-bitwuzla.exe"
    with pytest.raises(quodlibet.QuodlibetError):
        quodlibet.check(
            ADD, "add", SUM, "sum", solver_executable=str(missing)
        )


def test_an_explicit_argument_correspondence_is_accepted(backend):
    result = quodlibet.check(
        ADD,
        "add",
        SUM,
        "sum",
        argument_bindings=[(0, 1), (1, 0)],
        trust_smt_backend=True,
    )
    # add(x, y) against sum(b, a) with the arguments swapped is still the same
    # function, so the swapped correspondence proves too.
    assert result.verdict == "proved-equivalent"

    with pytest.raises(quodlibet.QuodlibetError):
        quodlibet.check(ADD, "add", SUM, "sum", argument_bindings=[(0, 0)])
