# W7 진행 기록: 안정성과 배포

브랜치: `w7-robustness-deploy` (원격에서는 `stuxnet147/w7-robustness-deploy`)
지시서: `docs/workstreams/W7.md`

## 지금 하는 것

항목 2 의 두 번째 절반. process transport fault-injection (가짜 solver 실행 파일로 crash, 잘린 출력, corrupt model, 폭주 출력, hang 주입).

## 착수 시 조사한 것 (2026-08-10)

### 기존 퍼징 표면

`tests/fuzz/fuzz_targets.h` 가 target 본체를 담고 두 소비자가 공유합니다.

- libFuzzer 드라이버 `tests/fuzz/fuzz_*.c` (`linux-fuzz` 프리셋에서만 빌드)
- 결정적 campaign `tests/test_fuzz.cpp` (모든 ctest 실행에서, Windows 포함)

착수 시점에 덮는 것은 세 개였습니다. `ql_fuzz_frontend` (C 파서), `ql_fuzz_lowering` (로어링 + IR verifier + interpreter), `ql_fuzz_ir_decoder` (IR artifact 디코더).

target 은 크래시만 보지 않고 불변식도 봅니다. `QL_FUZZ_REQUIRE` 로 "SUPPORTED 로어링은 반드시 verify 된다" 같은 것을 고정하고, `QL_FUZZ_REACHED` 로 target 도달 횟수를 세어 "아무것도 도달하지 못한 초록 실행" 을 실패로 만듭니다. 새 target 도 이 두 규율을 그대로 따릅니다.

### 빌드 배선

- 루트 `CMakeLists.txt` 175-185 행이 `tests/fuzz/fuzz_*.c` 를 glob 합니다. **새 드라이버는 파일만 만들면 targets 이 됩니다. CMake 를 고치지 않습니다.**
- `tests/CMakeLists.txt` 가 `test_*.cpp` 를 glob 합니다. 새 시험 파일도 만들기만 하면 됩니다.
- `scripts/coordinator/fuzz-campaign.sh` 가 `out/build/linux-fuzz/fuzz_*` 를 각 60초씩 돌립니다. Linux 전용입니다.

## 끝난 작업 단위

### 1. 퍼저 확대 (커밋 `d135f1e` 개설, 본체는 아래 커밋)

계약 표면 네 개에 퍼저를 붙였습니다. 계약 표면이란 "이 실행이 무엇을 주장해도 되는가" 를 정하는 입력입니다. 여기서는 잘못된 입력이 **받아들여지는 것**이 크래시보다 나쁩니다. 그래서 target 이 크래시만 보지 않고 그 표면이 존재하는 이유인 건전성 규칙 자체를 고정합니다.

새 파일:

- `tests/fuzz/fuzz_contract_targets.h` - target 본체
- `tests/fuzz/fuzz_precondition.c`, `fuzz_problem_decoder.c`, `fuzz_signature_decoder.c`, `fuzz_policy.c`, `fuzz_policy_result.c` - libFuzzer 드라이버
- `tests/test_fuzz_contracts.cpp` - 결정적 campaign 6개

고정한 불변식:

| target | 불변식 |
|---|---|
| precondition | 받아들인 계약의 canonical bytes 를 다시 parse 하면 같은 bytes 와 같은 digest 가 나온다 (같은 계약이 두 identity 를 갖지 못한다). node child index 가 전부 범위 안이다 |
| problem 디코더 | **schema v1 problem 은 어떤 byte 열로도 `ql_problem_require_proof_binding` 을 통과하지 못한다.** v1 은 v2 view 를 내지 못하고 source signature 를 갖지 못한다. v2 는 양쪽 signature 를 반드시 갖고 보고한 binding 이 전부 읽힌다 |
| source-signature 디코더 | argument 수가 schema 상한 이하, ABI 프로파일이 유효, pointer argument 폭이 signature 의 pointer width 와 일치, 파생한 precondition view 가 validate 된다 |
| policy | 받아들인 정책은 core 가 증명하지 않은 verdict 에 `proof` 를 주장하지 않고, replayed witness 를 요구하지 않은 채 counterexample 을 받지 않으며, proof trust 를 말하지 않은 채 proved verdict 를 pass 시키지 않는다. canonical form 이 고정점에 도달한다 |
| policy result | gated 결과는 effective verdict 가 unknown 이다. serialize/parse 왕복이 뜻을 바꾸지 않는다 |

측정한 도달 횟수 (Windows, ctest 1회):

```
precondition=10752  problem_v1=5724  problem_v2=2447
signature=1248      policy=6025      policy_result=22510
```

CTest 273/273 통과 (기존 267 + 신규 6). 드라이버 5개는 `clang -std=c17 -Wall -Wextra -Wpedantic -fsyntax-only` 통과. libFuzzer 실행 자체는 `linux-fuzz` 프리셋 전용이라 Windows 에서는 확인하지 못했습니다.

### 2. solver fault-injection, in-process 절반 (`tests/test_fault_injection.cpp`)

기존 solver 시험은 **정직하게 동작하는** backend 를 몹니다. 이 파일은 그렇지 않은 backend 를 몹니다. adapter 의 일이 오작동하거나 거짓말하는 backend 와 코어 사이에 서는 것이기 때문입니다.

먼저 이미 덮인 축을 조사해 빼놓았습니다.

| 이미 있는 것 | 어디 |
|---|---|
| cancellation, watchdog timeout 분류 | `SolverUnknownReason`, `Solver.MockSupportsSatModelAndCancellation` |
| SMT-LIB transcript 하드 상한 | `SmtLibBuilder.EnforcesHardTranscriptLimitBeforeAllocation`, `BitwuzlaSolver.EnforcesHardLimitOnCompleteTerminalQuery` |
| 상속된 pipe 가 직계 자식보다 오래 사는 경우 | `BitwuzlaTransport.BoundsInheritedPipesAfterDirectChildExit` |
| snapshot 변조 탐지 | `BitwuzlaSolver.RunsPrivateSnapshotAfterOverrideIsRemovedAndCleansIt` |
| decode 는 되지만 재현되지 않는 model | `tests/test_replay.cpp` 6개 |

더한 축은 **느린 backend 가 아니라 틀리게 답하는 backend** 입니다. 적대적 descriptor 하나가 16가지 거짓 결과를 냅니다. SAT 이 proof 를 들고 오기, UNSAT 이 model 을 들고 오기, UNKNOWN 이 증거를 들고 오기, 요청하지 않은 artifact, 라벨이 틀린 artifact, 요청한 model 누락, 영 backend digest, 잘못된 kind, kind 와 unknown reason 불일치, 범위 밖 unknown reason, 오류 반환과 동시에 artifact 보유.

**할당 계수기를 붙였습니다.** 오류 경로에서 artifact 를 남기면 sanitizer 빌드에서만 보이는 누수가 아니라 실패하는 시험이 됩니다. Windows 에서도 작동합니다. 거부된 check 를 같은 solver 에서 64회 반복해도 계수가 늘지 않는 것도 고정했습니다.

backend identity 를 descriptor 에서 찍는 것도 고정했습니다. backend 가 남의 이름으로 답하지 못해야 trusted-backend proof policy 의 key 가 뜻을 갖습니다.

CTest 278/278 통과 (직전 273 + 신규 5).

## 내린 설계 결정

- **새 target 을 `fuzz_targets.h` 가 아니라 별도 `fuzz_contract_targets.h` 에 둡니다.** 근거: `fuzz_targets.h` 와 `tests/test_fuzz.cpp` 는 W1 이 소유하는 표면(파서/로어링/IR)의 기록이고, W7 이 더하는 것은 계약 표면이라 소유가 다릅니다. `QL_FUZZ_REQUIRE`/`QL_FUZZ_REACHED` 규약은 그대로 따라서 두 헤더가 같은 규율 아래 있습니다.
- **precondition 퍼징은 signature 도 같이 흔듭니다.** `ql_precondition_parse` 가 signature 를 빌려 타입 검사를 하므로 JSON 만 흔들면 타입 검사 경로가 한 가지 signature 위에만 머뭅니다. 입력 앞부분을 signature descriptor 로 읽습니다.
- **mutator 의 절반은 JSON 유효성을 보존합니다.** 처음에는 bit flip 과 token 삽입만 썼는데 도달 횟수가 4만 회에 120 이었습니다. 거의 전부 keyword 안에 떨어져 syntax error 로 끝나고 타입 검사에는 닿지 못했기 때문입니다. 숫자 자리를 다른 숫자로 바꾸는 편집을 넣자 10752 로 올랐습니다. 이것이 width, argument index, alignment, bound 를 실제로 바꾸는 편집입니다.
- **binary artifact mutator 에도 gentle mode 를 넣습니다.** 앞 16 byte(디코더가 먼저 보는 header)를 건드리지 않고 1 bit 만 뒤집습니다. signature 도달이 50 에서 1248 로, problem v1 이 271 에서 5724 로 올랐습니다.
- **problem 디코더는 두 schema version 을 모두 시도합니다.** artifact header 와 payload 가 schema 를 각각 말하므로 한쪽만 선언하면 mutation 이 나머지 schema 에 영영 닿지 못합니다. 처음 구현에서 v1 도달이 0 이었던 원인입니다.
- **불변식 위반 시 입력 byte 열을 escape 해서 같이 보고합니다.** 문장만 보고하면 다음 사람이 campaign 전체를 다시 돌려야 어떤 입력이었는지 알 수 있습니다.

## 조율자에게 보고할 것

### result header 를 덮어쓴 backend 에서의 누수 (`src/solver.c`, W2 소유)

backend 가 `ql_solver_check_result_v1` 의 `abi_version` 이나 `struct_size` 를 덮어쓰면 `ql_solver_check` 는 `QL_STATUS_ABI_MISMATCH` 로 올바르게 거부합니다. 그러나 그 backend 가 할당한 artifact 는 해제되지 않습니다. 측정한 잔여 할당은 model artifact 하나당 3건입니다.

원인은 `src/solver.c:387` 의 `ql_solver_check_result_clear` 가 `abi_version` 과 `struct_size` 가 맞을 때만 해제한다는 것입니다. **바깥에서 들어온 layout 미상의 구조체에 대해서는 옳은 방어입니다.** 문제는 `ql_solver_check` 안에서는 layout 이 미상이 아니라는 점입니다. `local_result` 는 adapter 자신이 `ql_solver_check_result_init` 로 만든 것이고 backend 는 그 안의 필드만 덮어썼습니다. 지우기 전에 header 두 필드를 복원하면 해제됩니다.

`src/solver.c` 는 W2 소유라 고치지 않았습니다. **W7 이 고쳐도 되는지 판단이 필요합니다.**

시험은 이 누수를 정상으로 적지 않습니다. `RewrittenResultHeaderIsRejectedButStillLeaks` 가 잔여를 정확히 3으로 고정하므로 W2 가 고치면 이 시험이 실패하고 0 으로 조여집니다.


### 정규 형태가 고정점이 아닌 경우 (`src/policy.c`, W3 소유)

`ql_policy_serialize` 는 score `-0.0` 을 `-0` 으로 씁니다. 그 텍스트를 `ql_policy_parse` 로 다시 읽으면 yyjson 이 정수 0 으로 읽고, 다시 serialize 하면 `0` 이 나옵니다. 즉 `serialize(parse(serialize(p)))` 가 `serialize(p)` 와 다릅니다.

재현 (정책 하나, score 만 `-0.0`):

```
first  = ...,"score":-0}],"default_class":"open"}
second = ...,"score":0}],"default_class":"open"}
```

영향은 낮습니다. `-0.0` 과 `0.0` 은 수치로 같고 판정에 쓰이는 값이 아닙니다. 다만 헤더가 약속하는 canonical serialization 이 첫 왕복에서 안정되지 않는다는 뜻이고, 정책 텍스트를 digest 로 캐싱하면 같은 정책이 두 key 를 갖게 됩니다.

`src/policy.c` 는 W3 이 소유하므로 고치지 않았습니다. 고친다면 parse 에서 `-0.0` 을 `0.0` 으로 정규화하거나 writer 가 `-0.0` 을 명시적으로 쓰는 두 가지입니다. **W7 이 고쳐도 되는지 판단이 필요합니다.**

campaign 은 그동안 두 번째 serialize 부터의 고정점을 검사합니다. 이 한 단계를 넘는 불안정은 여전히 잡히고, 왜 그렇게 했는지는 `fuzz_contract_targets.h` 주석에 적혀 있습니다.

## 막힌 것

없습니다. 위 `-0.0` 건은 판단 대기이지 진행을 막지 않습니다.

## 다음에 할 것

1. (완료) 퍼저 확대
2. (진행 중) solver fault-injection 확대. in-process 절반 완료. 남은 절반은 process transport - 가짜 solver 실행 파일(테스트 바이너리를 env var 로 재실행)로 crash, 빈 출력, 잘린 출력, model 없는 SAT, corrupt model, 폭주 출력, hang 주입
3. 동시성 측정 -> `docs/perf/`. `bindings` 의 `check_batch` 를 부하 생성기로
4. ABI 호환 시험과 plugin SDK 예제
5. 설치 패키지와 relocatable Bitwuzla 탐색
6. compiler/target matrix 자동 검증 (`D:/projects/machine-model/datasets/records-local/summary.json`)
