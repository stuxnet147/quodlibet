"""Budgets and caller-defined verdict policies, as seen from Python."""

from __future__ import annotations

import pytest

import quodlibet

from conftest import TRUSTED_POLICY


def test_an_exhausted_budget_is_unknown_and_not_a_verdict(
    backend, equivalent_pair
):
    result = quodlibet.check(
        trust_smt_backend=True, budget={"total_ms": 1}, **equivalent_pair
    )
    assert result.verdict == "unknown"
    assert result.status.kind == "budget-exhausted"
    assert result.evidence.budget_exhausted is True
    assert result.evidence.budget_state == "total-time-exhausted"


def test_every_budget_axis_is_separately_settable(backend, equivalent_pair):
    generous = quodlibet.check(
        trust_smt_backend=True,
        budget={
            "total_ms": 120_000,
            "node_ms": 120_000,
            "solver_ms": 120_000,
            "memory_bytes": 1 << 30,
        },
        **equivalent_pair,
    )
    assert generous.verdict == "proved-equivalent"
    assert generous.evidence.budget_exhausted is False
    assert generous.evidence.usage["memory_peak_bytes"] > 0
    assert generous.evidence.usage["elapsed_ns"] > 0

    starved = quodlibet.check(
        trust_smt_backend=True,
        budget={"memory_bytes": 4096},
        **equivalent_pair,
    )
    assert starved.verdict == "unknown"
    assert starved.evidence.budget_exhausted is True
    assert starved.evidence.budget_state == "memory-exhausted"


def test_nanosecond_and_millisecond_spellings_do_not_mix(equivalent_pair):
    with pytest.raises(ValueError):
        quodlibet.check(
            budget={"total_ms": 1, "total_ns": 1}, **equivalent_pair
        )


def test_a_policy_that_would_manufacture_a_proof_is_refused(equivalent_pair):
    """The parser rejects it before the run starts, and it arrives as an
    exception rather than as a weaker verdict."""
    manufactured = {
        "schema_version": 1,
        "name": "cheat",
        "classes": [
            {
                "name": "accept",
                "disposition": "pass",
                "claims": "proof",
                "verdicts": ["bounded_clean"],
                "proof_trust": "trusted_backend",
            }
        ],
        "default_class": "accept",
        "trust": {"trusted_backends": ["bitwuzla 0.9.1"]},
    }
    with pytest.raises(quodlibet.QuodlibetError) as raised:
        quodlibet.check(policy_json=manufactured, **equivalent_pair)
    assert "claims" in str(raised.value)
    assert raised.value.status_name == "parse error"


def test_an_unreplayed_counterexample_class_is_refused(equivalent_pair):
    policy = {
        "schema_version": 1,
        "name": "cheat",
        "classes": [
            {
                "name": "reject",
                "disposition": "fail",
                "claims": "evidence",
                "verdicts": ["counterexample"],
            }
        ],
        "default_class": "reject",
    }
    with pytest.raises(quodlibet.QuodlibetError):
        quodlibet.check(policy_json=policy, **equivalent_pair)


def test_a_policy_classifies_a_real_proof(backend, equivalent_pair):
    result = quodlibet.check(
        trust_smt_backend=True, policy_json=TRUSTED_POLICY, **equivalent_pair
    )
    assert result.verdict == "proved-equivalent"
    assert result.policy is not None
    assert result.policy.passed
    assert result.policy.class_name == "accept"
    assert result.policy.claims == "proof"
    assert result.policy.score == 1.0
    assert result.policy.gated is False


def test_a_policy_cannot_pass_a_run_that_ran_out_of_budget(
    backend, equivalent_pair
):
    result = quodlibet.check(
        trust_smt_backend=True,
        policy_json=TRUSTED_POLICY,
        budget={"total_ms": 1},
        **equivalent_pair,
    )
    assert result.verdict == "unknown"
    assert result.policy.disposition == "abstain"
    assert result.policy.gated is True
    assert result.policy.gate_reason == "budget-exhausted"


def test_an_untrusted_backend_gates_a_proof(backend, equivalent_pair):
    policy = {
        "schema_version": 1,
        "name": "other-backend",
        "classes": [
            {
                "name": "accept",
                "disposition": "pass",
                "claims": "proof",
                "verdicts": ["proved_equivalent"],
                "proof_trust": "trusted_backend",
                "score": 1.0,
            },
            {
                "name": "open",
                "disposition": "abstain",
                "claims": "none",
                "verdicts": ["unknown"],
                "score": 0.0,
            },
        ],
        "default_class": "open",
        "trust": {"trusted_backends": ["some-other-solver 1.0"]},
    }
    result = quodlibet.check(
        trust_smt_backend=True, policy_json=policy, **equivalent_pair
    )
    assert result.verdict == "proved-equivalent"
    assert result.policy.gated is True
    assert result.policy.gate_reason == "untrusted-backend"
    assert result.policy.disposition == "abstain"
