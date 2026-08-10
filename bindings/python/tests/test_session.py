"""A solver session shares one backend installation across judgements.

These tests assert the contract, not a speedup. A wall-clock ratio would
measure the machine as much as the session; what has to hold is that the
verdicts are identical with and without one, that the per-judgement backend
digest is still reported, and that a session is closed deterministically.
"""

from __future__ import annotations

import threading

import pytest

import quodlibet

from conftest import ADD, SUM


def _spec(index: int) -> dict:
    """Distinct sources so nothing downstream can collapse a batch into one."""
    return {
        "left_source": f"int add(int x, int y){{ return x + y + {index} - {index}; }}",
        "left_function": "add",
        "right_source": f"int sum(int a, int b){{ return b + a + {index} - {index}; }}",
        "right_function": "sum",
        "trust_smt_backend": True,
    }


def test_a_session_does_not_change_the_verdict_or_the_digest(backend):
    """The point of a session is that it changes nothing except how often the
    backend is established."""
    without = quodlibet.check(**_spec(0))
    with quodlibet.SolverSession() as session:
        with_session = quodlibet.check(**_spec(0), session=session)

    assert without.verdict == with_session.verdict == "proved-equivalent"
    # The judgement still says which binary answered it. Sharing the install
    # did not stop the envelope recording it per judgement.
    assert with_session.evidence.solver_binary_digest
    assert (
        with_session.evidence.solver_binary_digest
        == without.evidence.solver_binary_digest
    )


def test_one_session_serves_many_judgements(backend):
    with quodlibet.SolverSession() as session:
        results = [
            quodlibet.check(**_spec(index), session=session)
            for index in range(4)
        ]
    assert [result.verdict for result in results] == [
        "proved-equivalent"
    ] * 4


def test_a_closed_session_is_refused_rather_than_ignored(backend):
    """Falling back silently would turn a lifetime bug into a quiet slowdown,
    so using a closed session is an error."""
    session = quodlibet.SolverSession()
    assert not session.closed
    session.close()
    assert session.closed

    with pytest.raises(ValueError):
        quodlibet.check(**_spec(0), session=session)


def test_closing_twice_is_harmless(backend):
    session = quodlibet.SolverSession()
    session.close()
    session.close()
    assert session.closed


def test_check_batch_gives_every_worker_its_own_session(backend, monkeypatch):
    """A session is not thread safe, so the batch must open one per worker and
    never share. This watches the sessions being made rather than trusting the
    code to have done it: it records which thread opened each one."""
    opened: list[int] = []
    lock = threading.Lock()
    real_session = quodlibet.SolverSession

    class WatchedSession(real_session):  # type: ignore[misc, valid-type]
        def __init__(self) -> None:
            super().__init__()
            with lock:
                opened.append(threading.get_ident())

    monkeypatch.setattr(quodlibet, "SolverSession", WatchedSession)

    workers = 4
    specs = [_spec(index) for index in range(16)]
    results = quodlibet.check_batch(specs, workers=workers)

    assert [result.verdict for result in results] == [
        "proved-equivalent"
    ] * len(specs)
    # One session per thread that ran work, never two threads on one session.
    assert opened, "the batch opened no session at all"
    assert len(opened) == len(set(opened)), (
        "a thread opened more than one session"
    )
    assert len(opened) <= workers


def test_check_batch_still_judges_when_sessions_are_unavailable(
    backend, monkeypatch
):
    """A backend with no session support must not break the batch; each check
    establishes its own installation, as it did before sessions existed."""

    def refuse() -> None:
        raise quodlibet.QuodlibetError("no session support in this build")

    monkeypatch.setattr(quodlibet, "SolverSession", refuse)

    results = quodlibet.check_batch([_spec(index) for index in range(4)],
                                    workers=2)
    assert [result.verdict for result in results] == [
        "proved-equivalent"
    ] * 4
