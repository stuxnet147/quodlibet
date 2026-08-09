"""Shared fixtures for the binding tests.

A test that needs the SMT backend states so explicitly. The suite never turns a
missing backend into a silent pass.
"""

from __future__ import annotations

import pytest

import quodlibet

ADD = "int add(int x, int y){ return x + y; }"
SUM = "int sum(int a, int b){ return b + a; }"
IDENTITY = "int f(int x){ return x; }"
SEVEN = "int g(int x){ if (x == 7) return 7; return 0; }"
DIVIDE = "int f(int x, int y){ return x / y; }"
GUARDED_DIVIDE = "int g(int a, int b){ if (b == 0) return 0; return a / b; }"
LOOPING = "int add(int x, int y){ while (y) { x = x + 1; y = y - 1; } return x; }"

TRUSTED_POLICY = {
    "schema_version": 1,
    "name": "rl-reward",
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
            "name": "reject",
            "disposition": "fail",
            "claims": "evidence",
            "verdicts": ["counterexample"],
            "require_replayed_witness": True,
            "score": -1.0,
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
    "trust": {"trusted_backends": ["bitwuzla 0.9.1"]},
}


@pytest.fixture(scope="session")
def backend() -> dict:
    info = quodlibet.backend_info()
    if not info["available"]:
        pytest.skip("this build has no Bitwuzla backend")
    return info


@pytest.fixture
def equivalent_pair() -> dict:
    return {
        "left_source": ADD,
        "left_function": "add",
        "right_source": SUM,
        "right_function": "sum",
    }
