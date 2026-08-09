"""The GIL is released for the whole judgement, and nothing the core owns
outlives the Python objects that borrowed it.

Both concurrency tests below assert a structural property rather than a
speedup ratio: whether other Python code ran at all while a check was in
flight, and whether two checks were in flight at the same instant. A wall-clock
ratio would measure the machine's load as much as the binding.
"""

from __future__ import annotations

import gc
import threading
import time
from concurrent.futures import ThreadPoolExecutor

import pytest

import quodlibet

from conftest import ADD, IDENTITY, SEVEN, SUM

WORKERS = 4


def _spec(index: int) -> dict:
    """Distinct sources so no cache can flatten the measurement."""
    return {
        "left_source": f"int add(int x, int y){{ return x + y + {index} - {index}; }}",
        "left_function": "add",
        "right_source": f"int sum(int a, int b){{ return b + a + {index} - {index}; }}",
        "right_function": "sum",
        "trust_smt_backend": True,
    }


def test_python_keeps_running_while_a_check_is_in_flight(backend):
    """A held GIL would freeze this counter for the whole call. Releasing it
    is what keeps a training loop's other threads alive."""
    stop = threading.Event()
    ticks = 0

    def spin() -> None:
        nonlocal ticks
        while not stop.is_set():
            ticks += 1

    ticker = threading.Thread(target=spin, daemon=True)
    ticker.start()
    try:
        before = ticks
        result = quodlibet.check(**_spec(0))
        during = ticks - before
    finally:
        stop.set()
        ticker.join(timeout=5)

    assert result.verdict == "proved-equivalent"
    # Six orders of magnitude of headroom over "the thread never ran".
    assert during > 10_000, (
        f"the ticker advanced only {during} times during a check; the GIL "
        "appears to be held across the judgement"
    )


def test_two_checks_are_in_flight_at_the_same_instant(backend):
    """Overlap is asserted from the intervals themselves. Serialized calls
    produce disjoint intervals no matter how fast the machine is."""
    intervals: list[tuple[float, float]] = []
    lock = threading.Lock()

    def timed(index: int) -> quodlibet.CheckResult:
        start = time.perf_counter()
        result = quodlibet.check(**_spec(index))
        end = time.perf_counter()
        with lock:
            intervals.append((start, end))
        return result

    with ThreadPoolExecutor(max_workers=WORKERS) as pool:
        results = list(pool.map(timed, range(WORKERS)))

    assert [result.verdict for result in results] == [
        "proved-equivalent"
    ] * WORKERS

    intervals.sort()
    concurrent = max(
        sum(1 for start, end in intervals if start <= point < end)
        for point, _ in intervals
    )
    assert concurrent >= 2, (
        f"no two of {intervals} overlapped; the GIL appears to be held "
        "across the judgement"
    )


def test_a_batch_returns_one_result_per_spec(backend):
    specs = [_spec(index) for index in range(WORKERS)]
    results = quodlibet.check_batch(specs, workers=WORKERS)
    assert [result.verdict for result in results] == [
        "proved-equivalent"
    ] * WORKERS


def test_an_empty_batch_and_a_bad_worker_count(backend):
    assert quodlibet.check_batch([]) == []
    with pytest.raises(ValueError):
        quodlibet.check_batch([_spec(0)], workers=-1)


def test_a_failing_spec_does_not_lose_the_rest_of_the_batch(backend):
    broken = _spec(0)
    broken["left_function"] = "absent"
    results = quodlibet.check_batch([broken, _spec(1)], workers=2)
    assert isinstance(results[0], quodlibet.QuodlibetError)
    assert results[1].verdict == "proved-equivalent"


def test_a_check_spec_object_is_accepted(backend):
    spec = quodlibet.CheckSpec(
        left_source=ADD,
        left_function="add",
        right_source=SUM,
        right_function="sum",
        trust_smt_backend=True,
    )
    (result,) = quodlibet.check_batch([spec], workers=1)
    assert result.verdict == "proved-equivalent"


def test_results_survive_collection_of_everything_that_made_them(backend):
    """Every core handle is released inside the call, and the counterexample
    bytes the result holds are a copy, so nothing here can dangle."""
    results = []
    for _ in range(8):
        result = quodlibet.check(
            IDENTITY,
            "f",
            SEVEN,
            "g",
            observations=["return-value"],
            trust_smt_backend=True,
        )
        results.append(result)
    gc.collect()
    counterexamples = [result.counterexample for result in results]
    del results
    gc.collect()
    for counterexample in counterexamples:
        assert counterexample["replayed"] is True
        assert counterexample["inputs"]
