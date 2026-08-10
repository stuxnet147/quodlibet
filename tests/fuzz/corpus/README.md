# Fuzz corpus seeds

One file per input that a fuzzer or a campaign found interesting enough to
keep. These are seeds, not regression tests: the property each one violated is
fixed by a named test, and the seed is here so a coverage-guided run starts
from a shape that already reached the interesting code rather than rediscovering
it.

Pass a directory to a libFuzzer driver to use it:

```sh
out/build/linux-fuzz/fuzz_policy tests/fuzz/corpus/policy -max_total_time=60
```

The deterministic campaign in `tests/test_fuzz_contracts.cpp` carries its own
seed list, which is what runs on every ctest. A seed worth keeping belongs in
both.

## policy/

- `negative-zero-score.json` - a score of `-0.0`. The canonical writer emitted
  `-0`, the JSON reader took it as the integer zero and dropped the sign, and
  the next write produced `0`, so the canonical form was not a fixed point of
  one round trip. Fixed in `src/policy.c` by collapsing both zeros; the
  property is held by `tests/test_policy_canonical.cpp`.
- `empty-input` - zero bytes. `ql_policy_parse` reads `json_size == 0` as a
  request to call `strlen` on the pointer (`src/policy.c:650`), so a
  zero-length slice of a buffer that is not NUL-terminated reads past the
  allocation. Found by the 2026-08-10 campaign
  (`docs/fuzz/campaign-20260810.md`) and **not yet fixed**.

## policy_result/

- `empty-input` - zero bytes. The same undocumented sentinel at
  `src/policy.c:1196` in `ql_policy_result_parse`. **Not yet fixed.**

The two `empty-input` seeds are the exception to the rule above: they are
kept while the defect is open, so neither seed has a named test holding its
property yet. They are deliberately absent from the seed list in
`tests/test_fuzz_contracts.cpp`, because adding a live crash to the
deterministic campaign would turn ctest red on a defect the fuzzing
workstream does not own. Whoever fixes `src/policy.c` should add both to that
list in the same change.
