# W7 진행 기록: 안정성과 배포

브랜치: `w7-robustness-deploy` (원격에서는 `stuxnet147/w7-robustness-deploy`)
지시서: `docs/workstreams/W7.md`

## 지금 하는 것

항목 5. 설치 패키지. Windows 와 Linux 에서 `cmake --install` 결과가 자기완결인지, 실행 파일 옆 relocatable Bitwuzla 탐색이 실제로 작동하는지.

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

### 3. solver fault-injection, process transport 절반 (`tests/test_fault_injection.cpp`)

Bitwuzla adapter 는 `executable` 옵션을 받아 그것이 가리키는 파일을 snapshot 합니다. 그래서 **이 테스트 바이너리의 복사본을 solver 로 세울 수 있습니다.** 어떤 고장을 낼지는 환경 변수로 정하므로 snapshot 하나로 모든 경우를 씁니다. 고장마다 새로 만들면 매번 테스트 실행 파일 전체를 복사하고 해시하게 됩니다.

자식은 version probe 와 실제 check 를 **stdin 으로** 구별합니다. adapter 는 `--version` 으로 probe 하면서 입력을 전혀 주지 않고, 실제 query 는 항상 `(exit)` 로 끝납니다. static constructor 에서 argv 를 읽는 것은 이식성이 없지만 stdin 을 끝까지 읽는 것은 이식성이 있습니다.

주입한 고장과 고정한 결과:

| 고장 | 결과 |
|---|---|
| 비정상 종료 (exit 3) | `QL_STATUS_METHOD_ERROR`, kind 는 `INVALID` |
| 출력 없음 | `METHOD_ERROR` "does not begin with" |
| 잘린 답 (`sa`) | `METHOD_ERROR` |
| 답 아닌 텍스트 | `METHOD_ERROR` |
| stdout 없이 stderr 만 | `QL_STATUS_PARSE_ERROR`, stderr 내용이 진단에 실림 |
| model 요청했는데 `sat` 만 | `METHOD_ERROR` "without the requested model" |
| corrupt model | check 는 OK, model artifact 가 **그 bytes 그대로**. NUL 포함해서 보존 |
| 폭주 출력 (16 MiB) | `stdout_limit_bytes` 에 걸려 `METHOD_ERROR`, artifact 없음, 시간 유계 |
| 응답 없음 (20초 sleep) | `UNKNOWN` + `UNKNOWN_TIMEOUT`, 300ms timeout 에서 1초 이내 복귀 |

corrupt model 을 그대로 나르는 것은 의도입니다. adapter 는 transport 이므로 여기서 그럴듯한 model 을 지어내거나 조용히 artifact 를 버리면 판단할 수 있는 층에게 오염을 숨기게 됩니다. 그 bytes 가 진짜 counterexample 인지는 replay 가 정하고 `tests/test_replay.cpp` 가 고정합니다.

측정한 시간은 Windows 에서 4개 합계 6.3초, 최대 4.1초입니다(CTest 케이스당 상한 30초). ASan 빌드는 계측된 실행 파일이 커서 snapshot 복사와 해시가 더 걸리므로 여유가 줄어듭니다. 기존 `BitwuzlaTransport.BoundsInheritedPipesAfterDirectChildExit` 도 같은 성질을 갖고 그 주석에 같은 취지가 적혀 있습니다.

CTest 282/282 통과 (직전 278 + 신규 4).

### 4. 동시성과 취소 비용 측정 (`scripts/perf/bench-concurrency.py`, `docs/perf/concurrency.md`)

**소유 겹침을 먼저 정리했습니다.** W7.md 는 `docs/perf/` 의 동시성 절을 W7 소유로, W8.md 는 `docs/perf/` 전부를 W8 소유로 적어 실제로 겹쳤습니다. 조율자에게 ask 로 올려 A 안(신규 `docs/perf/concurrency.md` 는 W7 소유, `baseline.md` 는 W8 유지, 링크 한 줄은 W8 이 연결)으로 확정했습니다. 스크립트는 벤치가 두 곳으로 갈리지 않도록 W8 의 관례를 따라 `scripts/perf/bench-concurrency.py` 입니다.

**W8 의 `bench-batch.py` 가 worker 수별 처리량과 occupancy 를 이미 덮습니다.** 그래서 W7 은 그것이 다루지 않는 축, 즉 예산 guard 비용과 취소 지연을 잽니다. 공개 파이썬 표면이 cancel token 이 아니라 예산을 노출하고 코어가 예산 소진 실행을 취소된 실행과 같은 경로로 UNKNOWN 회수하므로 예산이 취소의 관측 손잡이입니다.

**가장 쓸모 있는 결과: 취소 지연이 마감에 비례하지 않고 단계 경계로 양자화됩니다.**

| 예산 | median 복귀 |
|---|---|
| 1 ns | 0.0002s |
| 1 ms | 0.461s |
| 10 ms | 0.407s |
| 100 ms | 0.410s |
| 500 ms | 0.812s |
| 없음 | 0.862s (proved) |

1 ms, 10 ms, 100 ms 세 마감이 전부 같은 ~0.41초에 돌아옵니다. 예산 guard 가 solver 호출 안이 아니라 pipeline 단계 경계에서만 검사되기 때문입니다. 채점기에 뜻하는 바는 **100 ms 마감과 1 ms 마감이 같은 값이 든다**는 것입니다. 건전성은 전 구간 유지되어 초과 실행도 전부 `unknown` + `total-time-exhausted` 이고 `proved-*` 가 새지 않았습니다.

**arming 비용은 분해하지 못했고 그렇게 적었습니다.** 처음에는 median 만 찍어 workers=2 에서 42.8% 오버헤드가 나왔는데, min/max 를 같이 찍자 두 범위가 완전히 겹쳐 소음이었음이 드러났습니다. 그래서 하네스가 min/median/max 와 '범위 분리 여부'를 항상 찍도록 고쳤습니다. **median 하나는 재지 못한 것을 잰 것처럼 보이게 합니다.** workers=1 에서만 산포가 좁아(양쪽 ±6%) "직렬 실행에서 예산을 걸어 두는 것은 이 해상도에서 공짜" 라고 말할 수 있습니다.

worker 수별 처리량은 8 worker 4.31x 로 `baseline.md` 의 4.67x 와 같은 자리이며 교차 확인으로만 실었습니다.

측정 중 부수적으로 확인한 것: 빌드가 3.13 헤더로 만든 abi3 확장을 3.11 에서 호출하면 `SystemError: PY_SSIZE_T_CLEAN` 로 실패합니다. G4 가 닫은 방향(낮은 태그로 만들어 높은 인터프리터에서 사용)의 반대이므로 결함이 아니라 사용법이며 `concurrency.md` 재현 절에 적었습니다.

### 5. 공개 ABI 호환 시험 (`tests/test_abi_compat.cpp`)

계약을 문서가 아니라 시험이 들고 있게 했습니다. 세 가지입니다.

1. **더 오래된 헤더로 빌드된 caller 가 그대로 등록됩니다.** 필요한 prefix 크기만큼만 잡은 실제 버퍼에 descriptor 를 담아 넘깁니다. 패딩이 아니라 진짜 할당 경계라 `struct_size` 를 넘겨 읽으면 sanitizer 가 잡습니다.
2. **prefix 보다 1 byte 작으면 `ABI_MISMATCH` 로 거부되고** 진단이 무엇을 요구했는지 말합니다. ABI 세대가 다른 경우는 크기가 아니라 버전으로 거부되며 두 검사가 독립임을 고정합니다. 이 build 가 모르는 **더 큰** 구조체는 prefix 까지 읽고 받아들입니다. 거부하면 플러그인 하나가 새 헤더로 나올 때마다 host 업그레이드가 강제됩니다.
3. **기존 필드가 움직이지 않았음을 compile time 에 고정합니다.** `struct_size` 가 offset 0 인 것과 필드 순서를 `static_assert` 로 박았습니다. 크기 검사는 재배치를 잡지 못하므로 이것이 없으면 1 번과 2 번이 뜻을 잃습니다. 상대 순서로 박아 32/64bit 양쪽에서 성립합니다.

CMake 변경이 필요 없었습니다. 6개 시험 전부 통과.

### 6. plugin SDK 예제 (`examples/plugin/`, `tests/test_example_plugin.cpp`)

**예제를 저장소가 직접 빌드하고 시험이 로드합니다.** 조용히 컴파일이 깨진 예제, 또는 컴파일은 되지만 README 가 말하는 대로 동작하지 않는 예제는 없느니만 못합니다. 읽는 사람에게 디버깅 한 세션을 물리기 때문입니다. 조율자가 `596489f` 로 `examples/CMakeLists.txt` 가 있으면 빌드에 들어가는 훅을 루트에 넣어 주었습니다.

`normalize_plugin.c` 는 여전히 진짜인 최소 플러그인입니다. 하는 일(ASCII 공백 축약)은 일부러 지루하고, 보여 주는 것은 플러그인이 반드시 맞춰야 하는 네 가지이며 소스에 각각 번호로 표시했습니다.

1. **진입점이 ABI 악수 전부입니다.** `host->abi_version` 을 확인하고 아니면 `QL_STATUS_ABI_MISMATCH` 입니다.
2. **모든 구조체가 자기 크기와 세대를 말합니다.** 컴파일한 헤더의 `sizeof` 와 `QL_ABI_VERSION` 을 채웁니다.
3. **메모리는 host 를 통해서만 경계를 넘습니다.** host 와 플러그인이 서로 다른 CRT 에 링크될 수 있으므로 한쪽이 할당한 블록을 다른 쪽이 해제할 수 없습니다. 입력 artifact 는 빌린 것이고 `*output` 에 쓴 것은 caller 에게 넘어갑니다.
4. **감당할 수 있는 것만 선언합니다.** `QL_METHOD_PROOF_PRODUCER` 같은 flag 는 힌트가 아니라 건전성 주장입니다. 이 예제는 실제로 참인 `DETERMINISTIC` 과 `CACHEABLE` 만 답니다.

시험 5개가 README 의 약속을 그 순서대로 고정합니다. 마지막 것은 **`ql_plugin_load` 를 거치지 않고 플랫폼 로더로 모듈을 열어 진입점을 직접 부릅니다.** host 를 통해서는 이 빌드의 host 하나만 제시할 수 있어 거부 경로에 닿지 못하기 때문입니다. 다른 세대의 host, 너무 작은 host, null host 세 가지를 각각 다른 status 로 거부하는 것을 고정합니다.

모듈은 시험 실행 파일 옆에 떨어뜨려 시험이 자기 경로에서 찾습니다. `examples/` 가 `tests/` 보다 먼저 add 되어 `quodlibet_tests` 타깃이 아직 없으므로 의존을 걸 수 없고, 전체 빌드는 어차피 둘 다 만듭니다. 모듈이 없으면 시험은 skip 하지 않고 **무엇이 빌드되지 않았는지 말하며 실패합니다.**

CTest 435/435 통과.

## 내린 설계 결정

- **새 target 을 `fuzz_targets.h` 가 아니라 별도 `fuzz_contract_targets.h` 에 둡니다.** 근거: `fuzz_targets.h` 와 `tests/test_fuzz.cpp` 는 W1 이 소유하는 표면(파서/로어링/IR)의 기록이고, W7 이 더하는 것은 계약 표면이라 소유가 다릅니다. `QL_FUZZ_REQUIRE`/`QL_FUZZ_REACHED` 규약은 그대로 따라서 두 헤더가 같은 규율 아래 있습니다.
- **precondition 퍼징은 signature 도 같이 흔듭니다.** `ql_precondition_parse` 가 signature 를 빌려 타입 검사를 하므로 JSON 만 흔들면 타입 검사 경로가 한 가지 signature 위에만 머뭅니다. 입력 앞부분을 signature descriptor 로 읽습니다.
- **mutator 의 절반은 JSON 유효성을 보존합니다.** 처음에는 bit flip 과 token 삽입만 썼는데 도달 횟수가 4만 회에 120 이었습니다. 거의 전부 keyword 안에 떨어져 syntax error 로 끝나고 타입 검사에는 닿지 못했기 때문입니다. 숫자 자리를 다른 숫자로 바꾸는 편집을 넣자 10752 로 올랐습니다. 이것이 width, argument index, alignment, bound 를 실제로 바꾸는 편집입니다.
- **binary artifact mutator 에도 gentle mode 를 넣습니다.** 앞 16 byte(디코더가 먼저 보는 header)를 건드리지 않고 1 bit 만 뒤집습니다. signature 도달이 50 에서 1248 로, problem v1 이 271 에서 5724 로 올랐습니다.
- **problem 디코더는 두 schema version 을 모두 시도합니다.** artifact header 와 payload 가 schema 를 각각 말하므로 한쪽만 선언하면 mutation 이 나머지 schema 에 영영 닿지 못합니다. 처음 구현에서 v1 도달이 0 이었던 원인입니다.
- **불변식 위반 시 입력 byte 열을 escape 해서 같이 보고합니다.** 문장만 보고하면 다음 사람이 campaign 전체를 다시 돌려야 어떤 입력이었는지 알 수 있습니다.

## 조율자에게 보고할 것

### 병렬 CTest 에서 cache key 시험이 깨집니다 (`tests/test_cache_key.cpp`, W6 소유)

`ScopedRoot` 가 임시 디렉터리 이름을 **프로세스별 static counter** 로 만듭니다.

```cpp
static int counter = 0;
path_ = fs::temp_directory_path() /
        ("quodlibet-cache-key-test-" + std::to_string(++counter));
```

그런데 `gtest_discover_tests` 는 시험마다 **별도 프로세스**를 띄웁니다. 그래서 모든 프로세스가 counter 1 에서 시작해 전부 `quodlibet-cache-key-test-1` 을 씁니다. 병렬 CTest 에서 한 프로세스의 생성자가 다른 프로세스가 쓰는 중인 디렉터리를 `remove_all` 하고 소멸자가 또 지웁니다. 결과는 `ql_cache_load_artifact` 의 `QL_STATUS_NOT_FOUND` 이고 **매 실행마다 다른 시험이 깨집니다**.

기계가 한산하면 겹치는 창이 좁아 재현되지 않습니다. 부하가 걸린 상태에서 `ctest -R CacheKey` 를 세 번 돌려 세 번 모두 서로 다른 시험이 깨지는 것을 확인했고, 한산할 때는 세 번 모두 통과했습니다. 조율자의 통합 실행이 바쁜 기계에서 돌면 물립니다.

`tests/test_cache_key.cpp` 는 W6 소유라 고치지 않았습니다. 디렉터리 이름에 pid 를 넣으면 됩니다. `tests/test_solver.cpp` 의 snapshot prefix 가 이미 그 방식입니다.

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
2. (완료) solver fault-injection 확대. in-process 와 process transport 양쪽
3. (완료) 동시성 측정 -> `docs/perf/concurrency.md`
4. (완료) ABI 호환 시험과 plugin SDK 예제
5. (진행 중) 설치 패키지와 relocatable Bitwuzla 탐색
6. compiler/target matrix 자동 검증 (`D:/projects/machine-model/datasets/records-local/summary.json`)
