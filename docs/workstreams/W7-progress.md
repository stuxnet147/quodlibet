# W7 진행 기록: 안정성과 배포

브랜치: `w7-robustness-deploy` (원격에서는 `stuxnet147/w7-robustness-deploy`)
지시서: `docs/workstreams/W7.md`

## 지금 하는 것

항목 1. 퍼저 확대. `tests/fuzz/` 에 precondition 파서, problem v1/v2 디코더, source-signature 디코더, policy JSON 의 드라이버를 더합니다.

## 착수 시 조사한 것 (2026-08-10)

### 기존 퍼징 표면

`tests/fuzz/fuzz_targets.h` 가 target 본체를 담고 두 소비자가 공유합니다.

- libFuzzer 드라이버 `tests/fuzz/fuzz_*.c` (`linux-fuzz` 프리셋에서만 빌드)
- 결정적 campaign `tests/test_fuzz.cpp` (모든 ctest 실행에서, Windows 포함)

현재 덮는 것은 세 개입니다. `ql_fuzz_frontend` (C 파서), `ql_fuzz_lowering` (로어링 + IR verifier + interpreter), `ql_fuzz_ir_decoder` (IR artifact 디코더).

target 은 크래시만 보지 않고 불변식도 봅니다. `QL_FUZZ_REQUIRE` 로 "SUPPORTED 로어링은 반드시 verify 된다" 같은 것을 고정하고, `QL_FUZZ_REACHED` 로 target 도달 횟수를 세어 "아무것도 도달하지 못한 초록 실행" 을 실패로 만듭니다. 새 target 도 이 두 규율을 그대로 따릅니다.

### 빌드 배선

- 루트 `CMakeLists.txt` 175-185 행이 `tests/fuzz/fuzz_*.c` 를 glob 합니다. **새 드라이버는 파일만 만들면 targets 이 됩니다. CMake 를 고치지 않습니다.**
- `tests/CMakeLists.txt` 가 `test_*.cpp` 를 glob 합니다. 새 시험 파일도 만들기만 하면 됩니다.
- `scripts/coordinator/fuzz-campaign.sh` 가 `out/build/linux-fuzz/fuzz_*` 를 각 60초씩 돌립니다. Linux 전용입니다.

### 남은 표면 (W7 항목 1 의 대상)

| 표면 | 진입점 | 상태 |
|---|---|---|
| precondition JSON 파서 | `ql_precondition_parse` | 퍼저 없음 |
| problem 디코더 v1/v2 | `ql_problem_open` + view/binding 접근자 | 퍼저 없음 |
| source-signature 디코더 | `ql_source_signature_open` | 퍼저 없음 |
| policy JSON | `ql_policy_parse`, `ql_policy_result_parse` | 퍼저 없음 |

### 관측

`ql_precondition_parse` 는 signature 를 빌려 타입 검사를 합니다. 그래서 precondition 퍼징은 JSON 만 흔들면 절반만 흔드는 것입니다. signature 쪽도 같이 흔들어야 타입 검사 경로가 실제로 도달됩니다.

## 끝난 작업 단위

아직 없습니다.

## 내린 설계 결정

- 새 fuzz target 은 `tests/fuzz/fuzz_targets.h` 를 고치지 않고 별도 헤더 `tests/fuzz/fuzz_contract_targets.h` 에 둡니다. 근거: `fuzz_targets.h` 와 `tests/test_fuzz.cpp` 는 W1 이 소유하는 표면(파서/로어링/IR)의 기록이고, W7 이 더하는 것은 계약 표면(precondition/problem/signature/policy)이라 소유가 다릅니다. 같은 `QL_FUZZ_REQUIRE`/`QL_FUZZ_REACHED` 규약은 그대로 따릅니다.

## 막힌 것

없습니다.

## 다음에 할 것

1. (진행 중) 퍼저 확대
2. solver fault-injection 확대 (기존 커버 먼저 조사)
3. 동시성 측정 -> `docs/perf/`
4. ABI 호환 시험과 plugin SDK 예제
5. 설치 패키지와 relocatable Bitwuzla 탐색
6. compiler/target matrix 자동 검증
