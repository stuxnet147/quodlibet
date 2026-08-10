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
