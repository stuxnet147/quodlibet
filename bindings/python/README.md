# Quodlibet Python bindings

A CPython C extension over the Quodlibet function-equivalence engine, built for
use as a scorer and as a reward source inside a reinforcement-learning loop.

This is a compiled extension module, not an FFI. `ctypes` and `cffi`
re-declare a C ABI at run time from a Python-side description, and nothing
checks that the description still matches the library; a struct that grew a
field, a changed enum value, or a different calling convention becomes silent
memory corruption. Here the ABI is consumed by the C compiler from the same
`include/quodlibet/*.h` the core is built from, so a mismatch is a build error.

## Build and install

```sh
pip install ./bindings/python
```

`scikit-build-core` drives CMake, which configures the repository root as a
subproject and links the core **statically** into one self-contained module.
There is no companion shared library to place on a loader path.

The module is built against `Py_LIMITED_API` 3.11 (abi3), so a single wheel
serves 3.11 and every later CPython minor version. This is checked rather than
assumed: the `cp311-abi3` wheel built here is installed into a CPython 3.13
environment and the whole suite is run against it unchanged.

The distribution is named `quodlibet-engine` and the import name is
`quodlibet`. The import name is the engine's own, and it is not the unrelated
Quod Libet music player that publishes the `quodlibet` distribution; do not
install both into one environment.

Requirements: CMake 3.26 or later (for `Development.SABIModule`), a C17
compiler, and the vendored `third_party/` tree (`./scripts/vendor.sh`).

## Use

```python
import quodlibet

result = quodlibet.check(
    "int f(int a){ return a + a; }", "f",
    "int g(int a){ return 2 * a; }", "g",
    relation="equivalence",              # left-refines-right, right-refines-left
    ub_policy="both-defined",            # must-match, language-refinement
    trust_smt_backend=True,              # the explicit trusted-backend policy
    budget={"total_ms": 5000, "solver_ms": 3000, "memory_bytes": 1 << 30},
    policy_json=None,                    # a caller-defined verdict policy
)

result.verdict              # "proved-equivalent" | "counterexample" | "unknown" | ...
result.status               # how the run ended, separately from what it concluded
result.evidence             # the envelope summary, including checked_proof=False
result.counterexample       # the replayed inputs and observations, or None
result.loop_proof           # loop pairing, induction, summary, fallback telemetry
```

A batch for a training loop. The extension releases the GIL for the whole
judgement, so a thread pool is real parallelism:

```python
results = quodlibet.check_batch([spec1, spec2, ...], workers=0)  # 0 = cpu_count
```

`check_batch` returns a `QuodlibetError` in place of a result for a spec that
failed, so one bad pair never costs the rest of the batch.

## What the verdicts mean

The binding does not soften any of the core's boundaries.

* `trust_smt_backend=False` is the default. A solver `UNSAT` is then retained
  as evidence and the verdict stays `unknown`. Passing `True` selects the
  explicit, recorded trusted-backend policy, and `result.evidence.checked_proof`
  is still `False`, because the pinned Bitwuzla exposes no proof object for a
  checker to validate.
* `counterexample` is reported only after the SAT model was decoded into typed
  inputs and replayed concretely. `result.evidence.replay_confirmed` records it.
* Loop-free scalar code and the finite flat-memory slice use the ordinary SMT
  product. Paired reducible scalar loops may use the structural fast path.
  Exact whole-IR self-pairs may separately use digest-bound reflexivity when a
  concrete defined interpreter run establishes an inhabited domain.
* The induction path proves Base, guard alignment, Step, and Exit with one
  symbolic transition. It does not execute or finitely unroll the loop. A
  failed structural candidate records the unavailable CHC/PDR fallback and
  remains `unknown`. Affine summary opportunities are telemetry-only and no
  summary terminal is attempted.
* An exhausted budget is `unknown` with `result.evidence.budget_exhausted` set.
  Exhaustion is a run state, not a logical verdict.
* A verdict policy that would manufacture a proof, promote a verdict, or accept
  an unreplayed SAT model is rejected before the run starts, as a
  `QuodlibetError`.

`result.loop_proof` preserves raw per-judgement counts for loop discovery,
pairing, invariant generation, induction, summary, and fallback. Its
`stage_reached` map distinguishes a skipped stage from a measured zero; use
only applicable samples that actually reached and measured a stage when
computing latency percentiles. An unmeasured fallback duration is not zero.
A promoted loop proof still requires the explicit trusted-Bitwuzla policy and
either a satisfiable encoded comparison-domain query or the exact path's
concrete defined witness, and still reports `checked_proof=False`.

## The SMT backend

The core locates Bitwuzla 0.9.1 through the path compiled into it, which points
at the vendored bundle of the tree it was built from. Pass
`solver_executable="/absolute/path/to/bitwuzla"` to `check` to override it, for
instance when the wheel and the solver are deployed separately.
`quodlibet.backend_info()` reports what the module currently sees.

## Tests

```sh
pip install ./bindings/python
pytest bindings/python/tests
```

The suite is also registered with CTest as `quodlibet.python_bindings` when the
root project is configured and a CPython 3.11+ interpreter with pytest is
present. When one is not, the configure log states that the test was not
registered rather than registering a test that skips.
