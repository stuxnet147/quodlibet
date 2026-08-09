# Verdict policy

Different callers want different things from the same verdict. A
reinforcement-learning reward wants only proofs to count. A fast pre-filter
wants bounded-clean results to count as a pass, provided the record still says
they were bounded clean. `include/quodlibet/policy.h` lets the caller state
that in versioned JSON.

## What a policy can and cannot do

This is the line the whole design turns on:

> A policy decides **which already-justified verdicts count as a pass and what
> to call them**. It never manufactures a verdict.

The core decides what was proved. The policy decides what the caller does about
it. Those are different jobs, and a policy that reaches across the line is
rejected before the run starts.

Concretely, a policy may:

- group verdicts into named classes;
- give each class a disposition (`pass`, `fail`, `abstain`), a strength claim
  (`none`, `heuristic`, `evidence`, `proof`) and a numeric score;
- weaken a verdict to `unknown`;
- demand stronger evidence than the core's minimum before a class accepts a
  verdict.

A policy may not:

- call `bounded_clean` a proof;
- turn an unreplayed SAT model into a counterexample;
- turn a raw solver `unsat` into a proof with neither a checker nor a stated
  trusted backend;
- strengthen any verdict into any other verdict.

The parser enforces the first three as syntax, before anything runs.
`ql_policy_evaluate` enforces the same three again against the actual evidence,
because a well-formed policy still must not classify a verdict the evidence
does not support.

## Schema v1

```json
{
  "schema_version": 1,
  "name": "strict-proof-only",
  "description": "optional",
  "classes": [
    {
      "name": "accept",
      "disposition": "pass",
      "claims": "proof",
      "verdicts": ["proved_equivalent", "proved_left_refines_right"],
      "proof_trust": "checked",
      "score": 1.0
    },
    {
      "name": "refuted",
      "disposition": "fail",
      "claims": "evidence",
      "verdicts": ["counterexample"],
      "require_replayed_witness": true,
      "score": -1.0
    },
    {
      "name": "open",
      "disposition": "abstain",
      "claims": "none",
      "verdicts": ["unknown", "bounded_clean"],
      "score": 0.0
    }
  ],
  "default_class": "open",
  "weaken": [{ "from": "bounded_clean", "to": "unknown" }],
  "trust": { "trusted_backends": ["bitwuzla 0.9.1"] }
}
```

Verdict names are the lowercase forms of `ql_verdict`: `unknown`,
`proved_equivalent`, `proved_left_refines_right`, `proved_right_refines_left`,
`counterexample`, `bounded_clean`. `ql_policy_verdict_name` and
`ql_policy_verdict_parse` are the only spellings the schemas use, distinct from
the upper-case display names `ql_verdict_string` returns.

`schema_version` follows the same discipline as `precondition_json`: an exact
match is required and anything else is `QL_STATUS_SCHEMA_MISMATCH`.

## Rejection rules

Every rejection names its location: a JSON pointer for a semantic error, a byte
offset for malformed JSON.

| Rule | Rejected because |
| --- | --- |
| `claims: "proof"` on a class listing a non-proved verdict | this is how a policy would call `BOUNDED_CLEAN` a proof |
| a `weaken` entry whose `to` is neither `unknown` nor its own `from` | this is how a policy would promote a verdict outright |
| a class listing `counterexample` without `require_replayed_witness: true` | an unreplayed SAT model is not a counterexample |
| a class that passes or claims proof on a proved verdict without `proof_trust` | a raw solver `unsat` is not a proof |
| `proof_trust: "trusted_backend"` with an empty `trust.trusted_backends` | trusting no backend is the same promotion wearing a different hat |
| `proof_trust` on a class that lists no proved verdict | the field would be misleading |
| unknown key, duplicate class name, a verdict claimed by two classes, `default_class` naming nothing | ordinary structural errors |

A class that merely *abstains* on a proved verdict needs no `proof_trust`: it
banks nothing on the verdict and promotes nothing.

## Runtime gates

`ql_policy_evaluate` takes the outcome plus a `ql_policy_evidence_v1` carrying
facts only the core can know, and withdraws the effective verdict to `unknown`
before classification when:

| Gate reason | Condition |
| --- | --- |
| `budget-exhausted` | `evidence.budget_exhausted`, or `QL_OUTCOME_FLAG_BUDGET_EXHAUSTED` on the outcome |
| `unreplayed-sat-model` | the verdict is `counterexample` and no witness was replayed |
| `unchecked-proof` | the accepting class demands `checked` and no proof was checked |
| `untrusted-backend` | the accepting class demands `trusted_backend` and the reporting backend is not on the list |

A withdrawn verdict also loses `checked_bound`. The result keeps
`reported_verdict` alongside `effective_verdict`, so a caller can always see
what the core said and what the policy acted on. Only after the gates does the
`weaken` map apply, and only then does the surviving verdict select a class.

## Result schema v1

`ql_policy_result_v1` is self-contained: fixed-size name fields, no pointer
into the policy, so it can be copied, serialized, and handed to a binding.

```json
{
  "schema_version": 1,
  "policy": "strict-proof-only",
  "class": "open",
  "disposition": "abstain",
  "claims": "none",
  "reported_verdict": "proved_equivalent",
  "effective_verdict": "unknown",
  "weakened": false,
  "gated": true,
  "gate_reason": "unchecked-proof",
  "checked_bound": 0,
  "score": 0
}
```

Both writers report the exact size they need through `written` and refuse to
touch a short buffer, so a caller sizes first and writes second. Round trips
are fixed by tests in both directions: `ql_policy_serialize` output reparses
through `ql_policy_parse` into a policy that serializes byte-identically and
classifies identically, and `ql_policy_result_serialize` output reparses
through `ql_policy_result_parse` into a result that serializes byte-identically.

## Two policies, one verdict

`tests/test_policy.cpp` fixes the difference directly. Given the same
`BOUNDED_CLEAN` outcome:

| Policy | Class | Disposition | Claims | Effective verdict | Score |
| --- | --- | --- | --- | --- | --- |
| `strict-proof-only` | `open` | abstain | none | `bounded_clean` | 0.0 |
| `fast-filter` | `clean` | pass | heuristic | `bounded_clean` | 0.5 |

The fast filter counts it as a pass. It still does not call it a proof, and the
record still says `bounded_clean`. That is the whole permitted range of a
policy's authority.
