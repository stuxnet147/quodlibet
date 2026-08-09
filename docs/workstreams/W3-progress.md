# W3 진행 기록

브랜치: `stuxnet147/w3-runtime-services`
지시서: `docs/workstreams/W3.md`
담는 GOAL: G1(로깅), G2(예산), G3(판정 정책)

## 지금 하는 중

G1, G2, G3 세 목표의 작업 단위를 전부 마쳤습니다.

## 작업 단위

| # | 내용 | 상태 | 커밋 |
|---|---|---|---|
| 1 | 로깅 라이브러리 선정, 벤더링, SHA-256 고정, `DEPENDENCIES.md` 근거 | 완료 | 34c7ea5 |
| 2 | `include/quodlibet/log.h`, `src/log.c`, `tests/test_log.cpp` | 완료 | bf4c013 |
| 3 | 비활성 레벨 오버헤드 측정과 기록 | 완료 | 8f39116 |
| 4 | `include/quodlibet/budget.h`, `src/budget.c`, 할당자 계측 | 완료 | a0896a0 |
| 5 | 예산 훅과 판정 누출 방지 gate, ASan/UBSan | 완료 | a0896a0 |
| 6 | `include/quodlibet/policy.h`, `src/policy.c`, JSON 스키마와 왕복 | 완료 | |

## 설계 결정

### D1. 로깅 라이브러리는 zf_log v0.4.1

비교한 후보와 근거는 `DEPENDENCIES.md` 에 적습니다. 요약하면 MIT, 순수 C,
비활성 레벨이 전역 정수 한 번의 비교, 스택 버퍼로 메시지를 조립하고 콜백을
한 번만 호출, Windows 와 Linux 지원, 소스 2 파일 약 2300 줄입니다.

### D2. zf_log 를 별도 CMake 타깃으로 만들지 않고 `src/log.c` 안에서 컴파일

`CMakeLists.txt` 루트는 조율자가 소유하므로 새 vendor 타깃을 `quodlibet` 에
링크할 수 없습니다. `third_party/CMakeLists.txt` 는 `quodlibet` 타깃이 생기기
전에 처리되므로 그 안에서 링크를 걸 수도 없습니다.

그래서 `src/log.c` 가 설정 매크로를 정의한 뒤 벤더링된 `zf_log.c` 를 상대
경로로 `#include` 합니다. 단일 번역 단위 벤더링이며 루트 CMake 를 건드리지
않습니다. `third_party/CMakeLists.txt` 에는 필수 vendor 디렉터리 목록에
`zf_log` 만 추가해서 벤더링 누락이 configure 에서 잡히게 합니다.

**갱신(조율자 반영).** 조율자가 루트 `CMakeLists.txt` 에 `QL_ENABLE_LOGGING`
옵션을 정식으로 넣었고 단일 번역 단위 벤더링은 유지하기로 했습니다. 이제 로깅
OFF 구성은 `-DQL_ENABLE_LOGGING=OFF` 입니다.

### D3. 예산 초과에 새 `ql_status` 값을 만들지 않습니다

`include/quodlibet/status.h` 는 W3 소유가 아니고 다른 워크스트림이 지금 쓰고
있습니다. 새 열거값 대신 기존 의미를 그대로 씁니다.

- 메모리 예산 초과: `QL_STATUS_OUT_OF_MEMORY`
- 시간 예산 초과: `QL_STATUS_CANCELLED`

둘 다 기존 호출 경로가 이미 전파하고 정리하는 코드이므로 "실패가 호출 경로를
타고 깨끗하게 올라온다"는 요구가 새 코드 없이 만족됩니다. 정확한 사유는
`ql_budget_state()` 가 구분해서 알려줍니다.

## 다른 워크스트림에 대한 가정

- `include/quodlibet/semantics.h` 의 `ql_verdict` 와 `ql_outcome_v1` 은 읽기만
  합니다. W2 가 소유합니다.
- W2 의 solver 경로가 예산을 쓰려면 `ql_budget_remaining_ns()` 로 유효
  deadline 을 받아 `ql_solver_check_request_v1.timeout_ms` 에 넣습니다.
  W3 는 그 함수와 `ql_solver_is_cancelled_v1` 호환 콜백을 제공하고, 실제 배선은
  W2 쪽 코드가 준비되면 연결합니다.

### D6. 예산 축은 다섯 개이고 서로를 함의하지 않습니다

전체 wall-clock, 노드별 wall-clock, solver 호출별 wall-clock, 메모리 총량, 단일
할당 상한입니다. 0 은 그 축만 무제한입니다. 시간은 `uv_hrtime` 단조 시계라서
시스템 시계 조정이 예산을 늘리거나 줄이지 못합니다.

노드와 solver 축은 숨은 thread-local 상태가 아니라 명시적 `ql_budget_scope`
객체가 듭니다. 스케줄러가 한 예산 아래에서 노드를 병렬로 돌리기 때문입니다.

### D7. solver deadline 은 경쟁하지 않고 최솟값으로 합칩니다

`SOLVERS.md` 의 `timeout_ms` 는 백엔드로 가는 통로이고 예산 축은 천장입니다.
유효 deadline 은 호출자가 명시한 `timeout_ms`, 남은 solver scope, 남은 node
scope, 남은 total 중 최솟값입니다. `ql_budget_scope_remaining_ms` 가 그 값을
주고, 시간이 남아 있는 한 올림합니다. 백엔드가 0 을 "무제한"으로 읽기 때문에
1 ms 미만을 0 으로 잘라내면 deadline 이 조용히 사라집니다. 상세는
`docs/runtime-services/budget.md` 입니다.

### D8. `BOUNDED_CLEAN` 도 예산 초과와 함께 철회합니다

`ql_budget_guard_outcome` 이 `PROVED_*` 와 `COUNTEREXAMPLE` 뿐 아니라
`BOUNDED_CLEAN` 도 `UNKNOWN` 으로 되돌립니다. bounded clean 은 "명시한 bound 를
다 뒤졌고 반례가 없었다"는 주장인데 예산에 잘린 실행은 그 bound 를 다 뒤지지
않았으므로 그 주장을 할 수 없습니다.

### D9. 예산이 자기 자신의 누수 검출기입니다

Windows ASan 에는 LeakSanitizer 가 없습니다. 계측 할당자가 모든 블록을 정확히
세므로 `live_allocation_count` 와 `memory_current_bytes` 가 실행 전 값으로
돌아오는지가 그 자체로 정확한 누수 판정입니다. `PipelineBudgetLeak` 이 registry,
scheduler, pipeline, input 을 전부 계측 할당자 위에 올리고 노드 축으로 중단시킨
뒤 이것을 확인합니다. 진짜 LeakSanitizer 는 WSL Ubuntu-24.04 Linux ASan 실행이
담당합니다.

### D10. 정책은 분류만 하고 판정을 만들지 않습니다

정책이 할 수 있는 것은 이미 정당한 판정을 이름 붙인 class 로 묶고 그 class 에
disposition, claims, score 를 주는 것입니다. 판정을 `unknown` 으로 약화시키는
것까지는 허용하고 강화는 어떤 형태로도 금지합니다.

파서가 거부하는 네 가지가 요구된 세 가지 금지선을 그대로 문법으로 만듭니다.

- `claims: "proof"` class 가 proved 가 아닌 판정을 나열 -> `BOUNDED_CLEAN` 을
  proof 로 부르려는 시도
- `weaken` 의 `to` 가 `unknown` 도 `from` 도 아님 -> 판정 승격 시도
- `counterexample` 을 받는 class 에 `require_replayed_witness: true` 없음 ->
  replay 하지 않은 SAT model
- proved 를 pass 로 세거나 proof 를 주장하는 class 에 `proof_trust` 없음, 또는
  `trusted_backend` 인데 `trust.trusted_backends` 가 빔 -> checker 도
  trusted-backend policy 도 없는 raw UNSAT 승격

같은 규율을 `ql_policy_evaluate` 가 실행 시점에 실제 evidence 로 한 번 더
강제합니다. 형식이 맞는 정책이라도 증거가 없으면 분류하지 못합니다.

**abstain 하는 class 는 `proof_trust` 가 필요 없습니다.** 그 class 는 판정에
아무것도 걸지 않으므로 승격이 일어나지 않습니다. 처음에는 proved 를 나열하기만
해도 거부했는데 정상적인 정책까지 막아서 pass 이거나 `claims: "proof"` 인 경우로
좁혔습니다.

### D11. 정책 result 구조체는 포인터를 갖지 않습니다

`ql_policy_result_v1` 이 고정 길이 이름 배열을 씁니다. 정책 수명에 묶이지 않고
복사, 직렬화, 파이썬 노출이 전부 단순해집니다. W4 가 이 구조체를 그대로
넘깁니다.

### D4. `ql_log_level` 이름 충돌을 log.h 로 통합해서 해결

`include/quodlibet/method.h` 가 이미 plugin host log 콜백용으로 같은 이름의
열거형을 가지고 있었습니다. 값은 `QL_LOG_TRACE=0`, `DEBUG=1`, `INFO=2`,
`WARNING=3`, `ERROR=4` 였습니다.

`log.h` 의 새 열거형이 같은 자리에 같은 값을 두고(`TRACE=0` .. `ERROR=4`,
뒤에 `FATAL=5`, `OFF=6` 추가) `method.h` 는 정의를 지우고 `log.h` 를
include 한 다음 옛 철자를 매크로 별칭으로 남겼습니다. **수치가 동일하므로
plugin ABI 는 바뀌지 않습니다.** append-only 확장입니다.

같은 이유로 공개 로깅 매크로 이름은 `QL_LOG_TRACE(...)` 가 아니라
`QL_LOGT/QL_LOGD/QL_LOGI/QL_LOGW/QL_LOGE/QL_LOGF` 입니다.

### D5. plugin host 의 기본 log 콜백을 로깅 서비스로 돌립니다

`src/method.c` 의 `default_log` 가 무조건 `stderr` 로 찍고 있었습니다. G1 의
"라이브러리가 남의 stdout 을 마음대로 쓰지 않는다" 와 "기본값은 조용한 것" 을
정면으로 어기므로 `ql_log_write` 로 넘깁니다. 싱크를 안 걸면 조용합니다.

## 소유하지 않은 파일 중 손댄 것

조율자 확인이 필요한 항목입니다.

- `scripts/vendor.sh`: zf_log fetch 한 줄 추가. 벤더링에 필수.
- `THIRD_PARTY_NOTICES.md`: zf_log 행 추가.
- `include/quodlibet/quodlibet.h`: `log.h` include 한 줄 추가.
- `include/quodlibet/method.h`: D4. 열거형 정의를 `log.h` 로 옮기고 옛 철자를
  매크로 별칭으로 유지. 수치 불변.
- `src/method.c`: D5. `default_log` 를 `ql_log_write` 로 전환. `<stdio.h>`
  include 제거.
- `METHODS.md`: "Verdict discipline" 절 끝에 정책이 이 규율을 약화시킬 수
  없다는 문단과 `docs/runtime-services/` 문서 링크 추가.
- `include/quodlibet/pipeline.h`, `src/pipeline.c`: 예산 훅. `budget.h`
  include, `ql_pipeline_run_with_budget` 추가, `ql_pipeline_run` 은 budget NULL
  로 위임. 노드마다 `QL_BUDGET_SCOPE_NODE` scope 를 열고 닫으며 cancel 판정을
  token 과 예산 둘 다 보게 합니다. 기존 서명과 동작은 그대로입니다. 기능 확장은
  없습니다.

## 막힌 것

없음.

## 검증 기록

- 작업 단위 2 시점: `windows-clang` 113/113 통과, `QL_ENABLE_LOGGING=0` 별도
  build directory 에서도 113/113 통과. 두 구성의 `log.c.obj` 에서 zf_log
  심볼이 각각 19 개와 0 개로 확인됩니다.
- 작업 단위 3 시점: 두 구성 모두 114/114 통과. 비활성 레벨 호출 오버헤드는
  **0.071 ns/call** 로 측정되었고 연속 3 회 편차가 0.0005 ns 입니다. 측정
  방법과 기계 정보는 `docs/runtime-services/logging.md` 에 있습니다.
- 작업 단위 4, 5 시점(origin/main 784e719 위) 171/171 x 4 구성.
- 작업 단위 6 시점, origin/main 91a8143 위로 rebase 한 뒤 전 구성 확인:
  - `windows-clang` 239/239
  - `windows-clang` + `-DQL_ENABLE_LOGGING=OFF` 239/239
  - `linux-clang` (WSL Ubuntu-24.04, clang 18) 239/239
  - `linux-sanitize` (ASan+UBSan+LSan, Bitwuzla ON) 239/239. **누수 0.**

  조율자가 `linux-sanitize` 프리셋을 넣어 주어서 손으로 만들던 sanitizer build
  directory 대신 프리셋을 씁니다. 그 전 확인에서 쓰던 Windows ASan 구성은
  `QL_ENABLE_BITWUZLA=OFF` 라서 Bitwuzla 시험을 skip 했는데, `linux-sanitize`
  는 Bitwuzla 를 켠 채로 전 시험을 돌리므로 skip 없이 같은 범위를 덮습니다.

## 다음에 할 것

W3 지시서의 작업 단위는 전부 닫혔습니다. GOAL 의 G1, G2, G3 종료 조건 판정은
조율자가 합니다. 남은 연결 지점은 두 개입니다.

- W2 의 solver 경로가 `ql_budget_scope_remaining_ms` 로 유효 deadline 을 받아
  `ql_solver_check_request_v1.timeout_ms` 에 넣고, `cancel_state` 에 scope 를
  `is_cancelled` 에 `ql_budget_scope_is_cancelled` 를 넣는 배선. W3 쪽 함수는
  준비되어 있고 W2 의 product-program 경로가 생기면 연결하면 됩니다.
- W4 의 파이썬 바인딩이 `ql_budget_limits_v1`, `ql_policy`,
  `ql_policy_result_v1` 을 노출. 세 구조체 다 포인터 없이 값으로 다닐 수 있게
  설계했습니다.
