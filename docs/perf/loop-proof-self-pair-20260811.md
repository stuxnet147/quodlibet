# Loop self-pair proof 측정, 2026-08-11

## 결론

동일한 1,044개 validation self-pair에서 fresh baseline의 loop UNKNOWN
233개 중 133개를 `PROVED_EQUIVALENT`로 회수했습니다. 회수율은 57.08%이며
proof에서 bounded unrolling은 사용하지 않았습니다.

회수된 133개 표본은 exact whole-IR reflexivity를 사용했습니다. exact IR
artifact digest와 instruction 단위 structural identity가 보편적 동치의
근거이고, concrete interpreter의 defined 실행 한 건은 comparison domain이
비어 있지 않다는 사실만 증명합니다. 이 실행은 0, 1, all-ones scalar
pattern과 zero 또는 기존 object-base callee 결과를 유한하게 시도해 찾습니다.
후보 실행의 유한 탐색은 proof를 위한 loop unrolling이 아닙니다.

## 방법

- 환경: WSL Ubuntu 24.04, Clang Linux preset, Python 3.12.3
- backend: vendored Bitwuzla 0.9.1
- baseline core: detached worktree의
  `79d30264c9268f6fdad52f5648403def46d7ec7c`
- corpus: `out/corpus/val/detail-g9.tsv`, 1,044개 self-pair
- contract: equivalence, must-match UB, return, memory, termination, trap 관찰
- trust policy: explicit `trusted-backend`, `checked_proof=false`
- 실행: worker 1, 표본당 solver timeout 20,000ms
- percentile: nearest-rank, applicable loop outcome 중 해당 단계가 실제로
  reached 및 measured인 표본만 포함
- bounded unrolling: `false`

fresh WSL 자료는 다음과 같습니다.

- baseline:
  `out/loop-proof-20260811/before-wsl-79d30264.json` 및 `.jsonl`
- after:
  `out/loop-proof-20260811/after-final.json` 및 `.jsonl`

baseline과 after는 sample ID 1,044개가 모두 일치하며 누락과 신규 표본은
없습니다. baseline wall time은 841.718초, after wall time은 785.284초입니다.
전체 wall time은 proof coverage 지표일 뿐 단계 latency 대신 사용하지
않습니다.

사용자께서 언급하신 과거 18.5% 수치는 historical claim, raw unavailable로
분류합니다. 재현 가능한 비교에는 사용하지 않았으며, 위 fresh baseline을
같은 1,044개 표본으로 새로 실행했습니다. 기존 Windows baseline artifact와
workers=12 preliminary artifact도 최종 latency 계산에는 사용하지 않았습니다.

## Coverage와 proof 결과

| 지표 | 결과 |
|---|---:|
| 전체 probe | 1,044 |
| 명시적 결과 | 1,010 / 1,044, 96.74% |
| 전체 오류 | 34 / 1,044, 3.26% |
| applicable loop sample | 233 |
| natural-loop sample | 233 / 1,044, 22.32% |
| natural loop | 339 |
| noncanonical cycle | 0 / 233, 0.00% |
| loop pairing | 339 / 339, 100.00% |
| 모든 loop가 pairing된 표본 | 233 / 233, 100.00% |
| 완전한 structural invariant 생성 | 51 / 339 loop pair, 15.04% |
| SMT induction obligation UNSAT | 51 / 51 generated invariant, 100.00% |
| exact reflexivity proof | 188 / 339 loop pair, 55.46% |
| concrete domain witness | 133 / 233 sample, 57.08% |
| affine summary | 0 / 0 attempt, N/A |
| fallback reached | 59 / 233 sample, 25.32% |
| fallback attempted | 0 / 59 reached, 0.00% |
| 최종 loop UNKNOWN | 100 / 233, 42.92% |
| baseline loop UNKNOWN 회수 | 133 / 233, 57.08% |
| baseline proof에서 UNKNOWN으로 회귀 | 0 |

`invariant_generated_count`는 Base, guard alignment, Step, Exit를 모두 가진
완전한 induction query가 만들어진 loop pair만 셉니다. 41개 표본의 51개
invariant obligation이 UNSAT이었지만 concrete inhabited-domain witness를
찾지 못했으므로 최종 proof로 승격하지 않았습니다. 반대로 exact
reflexivity는 loop invariant를 합성했다고 주장하지 않으므로 invariant
분자에 넣지 않았습니다.

Affine recurrence와 closed-form opportunity는 분석 telemetry일 뿐입니다.
common iteration count와 실제 exit observable을 연결하는 terminal이 없으므로
summary query를 만들거나 시도하지 않았고, 성공률은 0/0인 N/A입니다.

## 오류 분모

오류가 난 record를 UNKNOWN 또는 coverage 분모에서 없애지 않았습니다.
전체 34개 오류 중 source에 loop syntax가 있는 오류는 18개입니다. 따라서
applicable outcome 233개와 이 오류 18개를 합친 loop population은 251개이며,
다음 두 비율을 별도로 기록합니다.

- source-loop error: 18 / 251, 7.17%
- loop UNKNOWN 또는 source-loop error: 118 / 251, 47.01%

나머지 전체 오류 16개도 전체 probe와 explicit-result 분모에는 남아
있습니다. 성공으로 취급하지 않았습니다.

## 남은 UNKNOWN 분류

| 원인 | 표본 |
|---|---:|
| ambiguous guard, 모든 concrete 후보 UB | 39 |
| induction obligation은 UNSAT이나 모든 concrete 후보 UB | 30 |
| induction obligation은 UNSAT이나 모든 concrete 후보 step limit | 11 |
| ambiguous guard, 모든 concrete 후보 step limit | 10 |
| nested loop, 모든 concrete 후보 UB | 5 |
| ambiguous guard, 모든 concrete 후보 unsupported | 3 |
| nested loop, 모든 concrete 후보 step limit | 2 |
| 합계 | 100 |

`promotion_eligible`을 완화하지 않았습니다. UB, unsupported semantics,
assumption rejection 또는 step limit에 도달한 concrete 후보는 domain
witness가 아니며, raw induction UNSAT도 이 gate를 넘지 못합니다.

## 단계별 latency

단위는 ms입니다. worker 1 after 실행에서 해당 단계에 실제 진입하고 시간이
측정된 applicable loop sample만 사용했습니다.

| 단계 | reached | measured | p50 | p95 |
|---|---:|---:|---:|---:|
| discover | 233 | 233 | 0.199 | 13.786 |
| canonicalize 및 concrete witness | 233 | 233 | 0.485 | 18.842 |
| pairing | 233 | 233 | 0.000294 | 0.001763 |
| invariant 및 query build | 233 | 233 | 0.0148 | 0.252 |
| induction solver | 41 | 41 | 14.016 | 190.329 |
| exact reflexivity solver | 133 | 133 | 5.690 | 7.650 |
| affine summary | 0 | 0 | N/A | N/A |
| CHC/PDR fallback | 59 | 0 | N/A | N/A |

CHC/PDR backend은 등록되어 있지 않습니다. fallback은 경계에 도달했다는
사실만 기록하며 실행 시간이 없으므로 0ms 실측값으로 보고하지 않습니다.

## Soundness 경계

- non-identical scalar path는 좌우 entry, guard, one-step transition, exit
  표현을 각각 직렬화한 실제 Base, guard alignment, Step, Exit induction을
  사용합니다.
- clean exact scalar path의 shared transition은 exact artifact 및 instruction
  structural identity가 먼저 성립할 때만 사용합니다.
- effect, UB, memory, pointer, nested 또는 ambiguous-guard exact self-pair는
  별도 whole-IR reflexivity terminal과 concrete defined witness를 사용합니다.
- candidate `SAT`은 invariant 후보를 거부할 뿐 counterexample이 아닙니다.
  replay되지 않은 model을 `COUNTEREXAMPLE`로 승격하지 않습니다.
- Bitwuzla proof object가 없으므로 결과는 explicit trusted-backend 정책
  아래에서도 `checked_proof=false`입니다.
- bounded unrolling은 어떤 `PROVED_*` 판정에도 사용하지 않습니다.

## 검증

최종 WSL 검증 결과는 다음과 같습니다.

- `./scripts/check.sh linux-clang`: native CTest 629 / 629 통과
- Python binding 전체 시험: 41 / 41 통과
- timeout, cancel, output cap, trust-none, exact solver-options cache identity,
  snapshot cleanup 집중 시험: 12 / 12 통과
- SAT invariant 후보, disconnected summary, failed domain witness 승격 금지
  집중 시험: 4 / 4 통과
- Bitwuzla OFF: 629개 중 557 통과, backend 의존 72개 명시적 skip,
  실패 0
- aggregate 자체 검증: sample ID 1,044개 일치, 누락 0, 신규 0,
  metric warning 0, 모든 count 불변식 통과

system Python에는 pytest가 없어 CMake가 Python CTest를 등록하지 않았습니다.
이 범위를 성공으로 숨기지 않고 pytest가 설치된 WSL venv와 Python 3.12.3으로
별도 41개 전체 binding 시험을 실행했습니다. WSL에는 Python 3.13 또는 3.11이
설치되어 있지 않습니다.

검증 명령은 아래와 같습니다.

```sh
git diff --check
./scripts/check.sh linux-clang
cmake --build out/build/linux-clang-bitwuzla-off --parallel
ctest --test-dir out/build/linux-clang-bitwuzla-off --output-on-failure
PYTHONPATH=out/build/linux-clang/bindings/python/package \
  out/venv-loop-proof/bin/python -m pytest -q bindings/python/tests
```

관련 변경만 대상으로 한 `git diff --check`는 통과했습니다. 전체 worktree의
`git diff --check`는 작업 전부터 존재한 다음 네 third-party CRLF 변경을
trailing-whitespace로 보고합니다. 이 파일들은 수정하거나 stage하지 않습니다.

- `third_party/libuv/docs/make.bat`
- `third_party/yyjson/test/data/json/test_encoding/utf8.json`
- `third_party/yyjson/test/data/json/test_encoding/utf8bom.json`
- `third_party/yyjson/test/data/json/test_yyjson/comment_singleline_end_crlf(comment).json`

최종 commit 대상은 별도로 `git diff --cached --check`를 통과시킵니다.

Windows 빌드와 시험은 수행하지 않았습니다. 이 작업은 사용자 지시에 따라
WSL 안에서만 검증했습니다.
