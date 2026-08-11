"""Python bindings for the Quodlibet function-equivalence engine.

The engine is reached through a CPython C extension (``quodlibet._quodlibet``)
compiled against the stable ABI, not through a runtime FFI redeclaration of the
C ABI. The extension releases the GIL for the whole judgement, so a batch runs
genuinely in parallel on a thread pool and a reinforcement-learning loop's other
threads are never starved while a solver is being waited on.

The vocabulary here is the core's vocabulary. In particular:

* ``unknown`` is a real answer. A source outside the restricted-C slice and a
  run that exhausted its budget both land there, and neither is silently
  narrowed into something stronger.
* ``proved-*`` is only reachable with ``trust_smt_backend=True``, because the
  pinned Bitwuzla exposes no proof object. ``result.evidence.checked_proof`` is
  then ``False`` and says so.
* ``counterexample`` is only reported after the SAT model was replayed
  concretely; ``result.evidence.replay_confirmed`` records that.

Judging more than one pair? Open a :class:`SolverSession` and pass it.
:func:`check_batch` does this per worker already; a caller looping over
:func:`check` has to ask, and the difference is about sixteen times per
judgement.
"""

from __future__ import annotations

import json
import os
import threading
from concurrent.futures import ThreadPoolExecutor
from dataclasses import dataclass, field
from typing import Any, Iterable, Mapping, Sequence

from . import _quodlibet

QuodlibetError = _quodlibet.QuodlibetError

__all__ = [
    "QuodlibetError",
    "CheckSpec",
    "CheckResult",
    "Evidence",
    "LoopProofStats",
    "Status",
    "PolicyResult",
    "check",
    "check_batch",
    "SolverSession",
    "backend_info",
    "core_version",
    "RELATIONS",
    "UB_POLICIES",
    "OBSERVATIONS",
    "VERDICTS",
]

RELATIONS: Mapping[str, int] = {
    "equivalence": _quodlibet.RELATION_EQUIVALENCE,
    "left-refines-right": _quodlibet.RELATION_LEFT_REFINES_RIGHT,
    "right-refines-left": _quodlibet.RELATION_RIGHT_REFINES_LEFT,
}

UB_POLICIES: Mapping[str, int] = {
    "must-match": _quodlibet.UB_MUST_MATCH,
    "language-refinement": _quodlibet.UB_LANGUAGE_REFINEMENT,
    "both-defined": _quodlibet.UB_COMPARE_WHERE_BOTH_DEFINED,
}

OBSERVATIONS: Mapping[str, int] = {
    "return-value": _quodlibet.OBSERVE_RETURN_VALUE,
    "memory": _quodlibet.OBSERVE_MEMORY,
    "termination": _quodlibet.OBSERVE_TERMINATION,
    "volatile": _quodlibet.OBSERVE_VOLATILE,
    "atomics": _quodlibet.OBSERVE_ATOMICS,
    "io": _quodlibet.OBSERVE_IO,
    "traps": _quodlibet.OBSERVE_TRAPS,
    "undefined-behavior": _quodlibet.OBSERVE_UNDEFINED_BEHAVIOR,
    "external-calls": _quodlibet.OBSERVE_EXTERNAL_CALLS,
}

VERDICTS: Mapping[int, str] = {
    _quodlibet.VERDICT_UNKNOWN: "unknown",
    _quodlibet.VERDICT_PROVED_EQUIVALENT: "proved-equivalent",
    _quodlibet.VERDICT_PROVED_LEFT_REFINES_RIGHT: "proved-left-refines-right",
    _quodlibet.VERDICT_PROVED_RIGHT_REFINES_LEFT: "proved-right-refines-left",
    _quodlibet.VERDICT_COUNTEREXAMPLE: "counterexample",
    _quodlibet.VERDICT_BOUNDED_CLEAN: "bounded-clean",
}

_ANSWERS: Mapping[int, str] = {
    _quodlibet.ANSWER_NOT_QUERIED: "not-queried",
    _quodlibet.ANSWER_SAT: "sat",
    _quodlibet.ANSWER_UNSAT: "unsat",
    _quodlibet.ANSWER_UNKNOWN: "unknown",
}

_OUTCOME_KINDS: Mapping[int, str] = {
    _quodlibet.OUTCOME_METHOD: "method",
    _quodlibet.OUTCOME_UNSUPPORTED: "unsupported",
    _quodlibet.OUTCOME_BUDGET: "budget-exhausted",
}

_BUDGET_KEYS = {
    "total_ms",
    "node_ms",
    "solver_ms",
    "total_ns",
    "node_ns",
    "solver_ns",
    "memory_bytes",
    "single_allocation_bytes",
}


@dataclass(frozen=True)
class Status:
    """How the run ended, separately from what it concluded."""

    kind: str
    message: str

    @property
    def ok(self) -> bool:
        """True when the proof method itself produced the verdict."""
        return self.kind == "method"

    def __str__(self) -> str:
        return f"{self.kind}: {self.message}" if self.message else self.kind


@dataclass(frozen=True)
class Evidence:
    """The evidence envelope summary, including what was *not* established."""

    evidence_class: str
    unsat_promotion: str
    violation_answer: str
    domain_answer: str
    checked_proof: bool
    replay_confirmed: bool
    budget_exhausted: bool
    budget_state: str
    problem_digest: str
    solver_query_digest: str
    solver_binary_digest: str
    cache_key: str
    counterexample_digest: str
    usage: Mapping[str, int] = field(default_factory=dict)


@dataclass(frozen=True)
class LoopProofStats:
    """Structured per-judgement loop fast-path telemetry.

    Counts retain their raw denominators. A ``stage_ns`` value is meaningful
    only when its matching ``stage_reached`` entry is true; reports must not
    mix skipped stages in as zero-latency samples. A reached fallback with a
    zero duration is the unavailable, unmeasured fallback boundary.
    """

    applicable: bool
    cyclic: bool
    natural_loop_count: int
    noncanonical_cycle: bool
    left_loop_count: int
    right_loop_count: int
    paired_loop_count: int
    all_loops_paired: bool
    invariant_generated_count: int
    induction_proved_count: int
    summary_attempted_count: int
    summary_proved_count: int
    reflexivity_proved_count: int
    fallback_reached: bool
    fallback_attempted: bool
    proof_eligible: bool
    concrete_domain_witness: bool
    strategy: str
    induction_answer: str
    summary_answer: str
    reflexivity_answer: str
    failure_reason: str
    stage_ns: Mapping[str, int] = field(default_factory=dict)
    stage_reached: Mapping[str, bool] = field(default_factory=dict)
    digests: Mapping[str, str] = field(default_factory=dict)


@dataclass(frozen=True)
class PolicyResult:
    """The caller's own verdict policy applied to the core's evidence."""

    policy_name: str
    class_name: str
    disposition: str
    claims: str
    score: float
    reported_verdict: str
    effective_verdict: str
    weakened: bool
    gated: bool
    gate_reason: str
    checked_bound: int

    @property
    def passed(self) -> bool:
        return self.disposition == "pass"


@dataclass(frozen=True)
class CheckResult:
    verdict: str
    status: Status
    evidence: Evidence
    counterexample: Any | None
    policy: PolicyResult | None
    diagnostic: str
    loop_proof: LoopProofStats
    #: Which registered method produced the final verdict:
    #: ``prove.smt-product`` unless a follow-up decided, then
    #: ``search.bounded-symbolic`` or ``prove.chc-pdr``. Empty on the paths
    #: that never reached a method (unsupported input, early budget).
    decided_by: str = "prove.smt-product"

    @property
    def proved(self) -> bool:
        return self.verdict.startswith("proved-")

    def __str__(self) -> str:
        return self.verdict


@dataclass
class CheckSpec:
    """One judgement request. ``check(**spec.as_kwargs())`` is the same call."""

    left_source: str
    left_function: str
    right_source: str
    right_function: str
    relation: str = "equivalence"
    ub_policy: str = "must-match"
    observations: Iterable[str] | int | None = None
    precondition_json: str | Mapping[str, Any] | None = None
    trust_smt_backend: bool = False
    budget: Mapping[str, int] | None = None
    policy_json: str | Mapping[str, Any] | None = None
    solver_executable: str | None = None
    solver_timeout_ms: int | None = None
    solver_memory_limit_mb: int = 0
    argument_bindings: Sequence[tuple[int, int]] | None = None
    #: The follow-up chain past an SMT-product ``unknown``: the retreating-edge
    #: bound search.bounded-symbolic unrolls to (0 skips the refuter), and
    #: whether prove.chc-pdr runs after it. Both on by default -- the loop
    #: territory only answers through them.
    bounded_unroll: int = 8
    chc_pdr: bool = True
    #: Reuse an already-established backend rather than building one for this
    #: judgement alone. See :class:`SolverSession`; a spec that leaves this
    #: null keeps the old behaviour, and one handed to :func:`check_batch`
    #: gets that worker's session instead.
    session: "SolverSession | None" = None

    def as_kwargs(self) -> dict[str, Any]:
        return {
            "left_source": self.left_source,
            "left_function": self.left_function,
            "right_source": self.right_source,
            "right_function": self.right_function,
            "relation": self.relation,
            "ub_policy": self.ub_policy,
            "observations": self.observations,
            "precondition_json": self.precondition_json,
            "trust_smt_backend": self.trust_smt_backend,
            "budget": self.budget,
            "policy_json": self.policy_json,
            "solver_executable": self.solver_executable,
            "solver_timeout_ms": self.solver_timeout_ms,
            "solver_memory_limit_mb": self.solver_memory_limit_mb,
            "argument_bindings": self.argument_bindings,
            "bounded_unroll": self.bounded_unroll,
            "chc_pdr": self.chc_pdr,
            "session": self.session,
        }


def _as_json(value: str | Mapping[str, Any] | None) -> str | None:
    if value is None or isinstance(value, str):
        return value
    return json.dumps(value)


def _observation_mask(observations: Iterable[str] | int | None) -> int:
    if observations is None:
        return 0
    if isinstance(observations, int):
        return observations
    mask = 0
    for name in observations:
        if name not in OBSERVATIONS:
            raise ValueError(
                f"unknown observation {name!r}; known: "
                f"{sorted(OBSERVATIONS)}"
            )
        mask |= OBSERVATIONS[name]
    return mask


def _budget_ns(budget: Mapping[str, int] | None) -> dict[str, int]:
    """Millisecond keys are the friendly spelling; the core axes are ns."""
    if budget is None:
        return {
            "total_wall_clock_ns": 0,
            "node_wall_clock_ns": 0,
            "solver_wall_clock_ns": 0,
            "memory_bytes": 0,
            "single_allocation_bytes": 0,
        }
    unknown = set(budget) - _BUDGET_KEYS
    if unknown:
        raise ValueError(
            f"unknown budget keys {sorted(unknown)}; known: "
            f"{sorted(_BUDGET_KEYS)}"
        )

    def axis(ms_key: str, ns_key: str) -> int:
        if ms_key in budget and ns_key in budget:
            raise ValueError(f"give either {ms_key} or {ns_key}, not both")
        if ns_key in budget:
            return int(budget[ns_key])
        return int(budget.get(ms_key, 0)) * 1_000_000

    return {
        "total_wall_clock_ns": axis("total_ms", "total_ns"),
        "node_wall_clock_ns": axis("node_ms", "node_ns"),
        "solver_wall_clock_ns": axis("solver_ms", "solver_ns"),
        "memory_bytes": int(budget.get("memory_bytes", 0)),
        "single_allocation_bytes": int(
            budget.get("single_allocation_bytes", 0)
        ),
    }


def _build_result(raw: Mapping[str, Any]) -> CheckResult:
    kind = _OUTCOME_KINDS[raw["outcome_kind"]]
    evidence = Evidence(
        evidence_class=raw["evidence_class_name"],
        unsat_promotion=(
            "trusted-backend" if raw["unsat_promotion"] else "none"
        ),
        violation_answer=_ANSWERS[raw["violation_answer"]],
        domain_answer=_ANSWERS[raw["domain_answer"]],
        checked_proof=raw["checked_proof"],
        replay_confirmed=raw["replay_confirmed"],
        budget_exhausted=raw["budget_exhausted"],
        budget_state=raw["budget_state"],
        problem_digest=raw["problem_digest"],
        solver_query_digest=raw["solver_query_digest"],
        solver_binary_digest=raw["solver_binary_digest"],
        cache_key=raw["cache_key"],
        counterexample_digest=raw["counterexample_digest"],
        usage=dict(raw["usage"]),
    )
    counterexample = None
    if raw["counterexample_json"] is not None:
        counterexample = json.loads(raw["counterexample_json"])
    policy = None
    if raw["policy"] is not None:
        entry = raw["policy"]
        policy = PolicyResult(
            policy_name=entry["policy_name"],
            class_name=entry["class_name"],
            disposition=entry["disposition"],
            claims=entry["claims"],
            score=entry["score"],
            reported_verdict=VERDICTS[entry["reported_verdict"]],
            effective_verdict=VERDICTS[entry["effective_verdict"]],
            weakened=entry["weakened"],
            gated=entry["gated"],
            gate_reason=entry["gate_reason"],
            checked_bound=entry["checked_bound"],
        )
    loop = raw["loop_proof"]
    loop_proof = LoopProofStats(
        applicable=bool(loop["applicable"]),
        cyclic=bool(loop["cyclic"]),
        natural_loop_count=int(loop["natural_loop_count"]),
        noncanonical_cycle=bool(loop["noncanonical_cycle"]),
        left_loop_count=int(loop["left_loop_count"]),
        right_loop_count=int(loop["right_loop_count"]),
        paired_loop_count=int(loop["paired_loop_count"]),
        all_loops_paired=bool(loop["all_loops_paired"]),
        invariant_generated_count=int(loop["invariant_generated_count"]),
        induction_proved_count=int(loop["induction_proved_count"]),
        summary_attempted_count=int(loop["summary_attempted_count"]),
        summary_proved_count=int(loop["summary_proved_count"]),
        reflexivity_proved_count=int(loop["reflexivity_proved_count"]),
        fallback_reached=bool(loop["fallback_reached"]),
        fallback_attempted=bool(loop["fallback_attempted"]),
        proof_eligible=bool(loop["proof_eligible"]),
        concrete_domain_witness=bool(loop["concrete_domain_witness"]),
        strategy=str(loop["strategy"]),
        induction_answer=_ANSWERS[int(loop["induction_answer"])],
        summary_answer=_ANSWERS[int(loop["summary_answer"])],
        reflexivity_answer=_ANSWERS[int(loop["reflexivity_answer"])],
        failure_reason=str(loop["failure_reason"]),
        stage_ns=dict(loop["stage_ns"]),
        stage_reached=dict(loop["stage_reached"]),
        digests={
            "canonical": str(loop["canonical_digest"]),
            "base": str(loop["base_obligation_digest"]),
            "guard": str(loop["guard_obligation_digest"]),
            "step": str(loop["step_obligation_digest"]),
            "exit": str(loop["exit_obligation_digest"]),
            "reflexivity": str(loop["reflexivity_obligation_digest"]),
            "domain_witness": str(loop["domain_witness_digest"]),
        },
    )
    return CheckResult(
        verdict=VERDICTS[raw["verdict"]],
        status=Status(kind=kind, message=raw["diagnostic"]),
        evidence=evidence,
        counterexample=counterexample,
        policy=policy,
        diagnostic=raw["diagnostic"],
        loop_proof=loop_proof,
        decided_by=raw.get("decided_by") or "",
    )


class SolverSession:
    """One private backend installation, reused across many judgements.

    Establishing the backend costs more than answering with it: the adapter
    copies the solver executable somewhere private, hashes it and probes its
    version before the first query. A session pays that once instead of once
    per :func:`check`.

    It changes nothing about what is verified. The snapshot is still hashed
    before and during every check against the digest taken when the session
    opened, and every result still reports the backend digest that answered
    it.

    **Not thread safe, by contract.** One session per worker thread. The state
    it hands out is single-threaded, so a shared session would let two
    judgements interleave inside one solver. :func:`check_batch` gives each of
    its workers its own; code that builds its own pool must do the same.

    Use it as a context manager, or call :meth:`close`. Closing drops the only
    reference to the handle, and the installation is removed as that handle is
    finalised rather than waiting for interpreter exit.
    """

    __slots__ = ("_capsule",)

    def __init__(self) -> None:
        self._capsule = _quodlibet.solver_session_open()

    @property
    def closed(self) -> bool:
        return self._capsule is None

    def close(self) -> None:
        """Tear the installation down. Idempotent."""
        self._capsule = None

    def _handle(self) -> object:
        """The capsule, or a refusal. Using a closed session must not quietly
        fall back to establishing a backend per check: that would turn a
        lifetime bug into a silent slowdown."""
        if self._capsule is None:
            raise ValueError("solver session is closed")
        return self._capsule

    def __enter__(self) -> "SolverSession":
        return self

    def __exit__(self, *exc_info: object) -> None:
        self.close()


def check(
    left_source: str,
    left_function: str,
    right_source: str,
    right_function: str,
    *,
    relation: str = "equivalence",
    ub_policy: str = "must-match",
    observations: Iterable[str] | int | None = None,
    precondition_json: str | Mapping[str, Any] | None = None,
    trust_smt_backend: bool = False,
    budget: Mapping[str, int] | None = None,
    policy_json: str | Mapping[str, Any] | None = None,
    solver_executable: str | None = None,
    solver_timeout_ms: int | None = None,
    solver_memory_limit_mb: int = 0,
    argument_bindings: Sequence[tuple[int, int]] | None = None,
    bounded_unroll: int = 8,
    chc_pdr: bool = True,
    session: "SolverSession | None" = None,
) -> CheckResult:
    """Judge one pair of restricted-C functions.

    ``trust_smt_backend=True`` selects the recorded trusted-backend policy;
    without it a solver UNSAT stays evidence and the verdict stays
    ``unknown``. Budget exhaustion arrives as ``unknown`` with
    ``evidence.budget_exhausted`` set, never as a logical verdict. A malformed
    verdict policy raises before the run starts.

    When the SMT product fails to *answer* -- ``unknown`` with the violation
    query not answered UNSAT and the domain not vacuous, which is the loop
    territory and the solver-timeout territory, not the trust-policy
    territory -- the check continues down a follow-up chain:
    ``search.bounded-symbolic`` unrolls
    both sides ``bounded_unroll`` retreating-edge traversals deep and hunts a
    counterexample (only a model replayed on the original cyclic functions is
    one; a clean bound comes back as ``bounded-clean`` and is never promoted),
    then ``prove.chc-pdr`` attempts an inductive proof over the serialized
    loop transition prefix (promoted only under the same trusted-backend
    policy as the product's UNSAT). ``result.decided_by`` names the method
    whose verdict came back; ``evidence.violation_answer`` and
    ``evidence.domain_answer`` always describe the SMT-product stage.
    ``bounded_unroll=0`` and ``chc_pdr=False`` restore the bare product.

    **Pass a session when you judge more than once.** Without one this call
    establishes a private backend installation of its own, and it does so
    twice, because a judgement asks the solver two questions. Measured at
    1,093.8 ms against 66.2 ms per judgement, so a caller in a loop pays
    about sixteen times over::

        with quodlibet.SolverSession() as session:
            for left, right in pairs:
                result = quodlibet.check(..., session=session)

    :func:`check_batch` already opens one per worker; this is for callers
    driving :func:`check` themselves. The session is explicit rather than
    implicit because it is not thread safe: one per thread, never shared. A
    session changes nothing about the answer, only about how often the
    backend is built, and every result still reports the backend digest that
    produced it.
    """
    if relation not in RELATIONS:
        raise ValueError(
            f"unknown relation {relation!r}; known: {sorted(RELATIONS)}"
        )
    if ub_policy not in UB_POLICIES:
        raise ValueError(
            f"unknown ub_policy {ub_policy!r}; known: {sorted(UB_POLICIES)}"
        )

    mask = _observation_mask(observations)
    memory_observation = (
        _quodlibet.MEMORY_FINAL_REACHABLE_STATE
        if mask & _quodlibet.OBSERVE_MEMORY
        else _quodlibet.MEMORY_IGNORE
    )
    external_call_observation = (
        _quodlibet.EXTERNAL_CALLS_ORDERED_TRACE
        if mask & _quodlibet.OBSERVE_EXTERNAL_CALLS
        else _quodlibet.EXTERNAL_CALLS_IGNORE
    )
    limits = _budget_ns(budget)
    if solver_timeout_ms is None:
        # A solver that outlives the budget's solver axis would only be
        # cancelled by polling, so the axis is also handed to the backend.
        solver_timeout_ms = limits["solver_wall_clock_ns"] // 1_000_000

    raw = _quodlibet.check(
        left_source=left_source,
        left_function=left_function,
        right_source=right_source,
        right_function=right_function,
        relation=RELATIONS[relation],
        ub_policy=UB_POLICIES[ub_policy],
        observations=mask,
        memory_observation=memory_observation,
        external_call_observation=external_call_observation,
        precondition_json=_as_json(precondition_json),
        trust_smt_backend=bool(trust_smt_backend),
        solver_timeout_ms=int(solver_timeout_ms),
        solver_memory_limit_mb=int(solver_memory_limit_mb),
        solver_executable=solver_executable,
        policy_json=_as_json(policy_json),
        argument_bindings=(
            None
            if argument_bindings is None
            else [tuple(pair) for pair in argument_bindings]
        ),
        solver_session=None if session is None else session._handle(),
        bounded_unroll=int(bounded_unroll),
        chc_pdr=bool(chc_pdr),
        **limits,
    )
    return _build_result(raw)


def check_batch(
    specs: Sequence[CheckSpec | Mapping[str, Any]],
    workers: int = 0,
) -> list[CheckResult | QuodlibetError]:
    """Judge many pairs concurrently.

    The extension releases the GIL for each judgement, so a plain thread pool
    is real parallelism here. ``workers=0`` means ``os.cpu_count()``. A spec
    that raises is returned in place as its exception rather than losing the
    whole batch, because a scorer must still score the rest.
    """
    if not specs:
        return []
    if workers < 0:
        raise ValueError("workers must be zero or positive")
    count = workers or (os.cpu_count() or 1)
    count = min(count, len(specs))

    # One session per worker, never shared: a session is not thread safe, and
    # thread-local storage is what makes "per worker" true no matter how the
    # pool assigns work. The sessions are closed together at the end rather
    # than per task, which is the point -- a session closed after every
    # judgement would establish the backend as often as having none at all.
    local = threading.local()
    sessions: list[SolverSession] = []
    sessions_lock = threading.Lock()

    def worker_session() -> SolverSession | None:
        existing = getattr(local, "session", None)
        if existing is not None:
            return existing
        try:
            created = SolverSession()
        except QuodlibetError:
            # A backend that cannot open a session still judges; each check
            # establishes its own installation, as it did before sessions.
            local.session = None
            return None
        local.session = created
        with sessions_lock:
            sessions.append(created)
        return created

    def run(spec: CheckSpec | Mapping[str, Any]) -> CheckResult | QuodlibetError:
        kwargs = spec.as_kwargs() if isinstance(spec, CheckSpec) else dict(spec)
        # A spec that names its own session keeps it, and no worker session is
        # opened for it: setdefault would have built one either way.
        if kwargs.get("session") is None:
            kwargs["session"] = worker_session()
        try:
            return check(**kwargs)
        except QuodlibetError as error:
            return error

    try:
        if count == 1:
            return [run(spec) for spec in specs]
        with ThreadPoolExecutor(max_workers=count) as pool:
            return list(pool.map(run, specs))
    finally:
        for session in sessions:
            session.close()


def backend_info() -> dict[str, Any]:
    """The pinned SMT backend this extension was built against."""
    return _quodlibet.backend_info()


def core_version() -> str:
    return _quodlibet.core_version()
