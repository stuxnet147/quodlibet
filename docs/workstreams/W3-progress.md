# W3 진행 기록

브랜치: `stuxnet147/w3-runtime-services`
지시서: `docs/workstreams/W3.md`
담는 GOAL: G1(로깅), G2(예산), G3(판정 정책)

## 지금 하는 중

G1 완료. 다음은 G2 예산.

## 작업 단위

| # | 내용 | 상태 | 커밋 |
|---|---|---|---|
| 1 | 로깅 라이브러리 선정, 벤더링, SHA-256 고정, `DEPENDENCIES.md` 근거 | 완료 | 34c7ea5 |
| 2 | `include/quodlibet/log.h`, `src/log.c`, `tests/test_log.cpp` | 완료 | bf4c013 |
| 3 | 비활성 레벨 오버헤드 측정과 기록 | 완료 | |
| 4 | `include/quodlibet/budget.h`, `src/budget.c`, 할당자 계측 | 진행 | |
| 5 | 예산 훅과 판정 누출 방지 gate, ASan/UBSan | 대기 | |
| 6 | `include/quodlibet/policy.h`, `src/policy.c`, JSON 스키마와 왕복 | 대기 | |

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

조율자가 나중에 `ql_vendor_zf_log` 타깃과 `QL_ENABLE_LOGGING` 옵션을 루트에
넣기로 하면 그때 정식 타깃으로 옮깁니다. 그 전까지 로깅 OFF 구성은 별도 build
directory 에서 `-DCMAKE_C_FLAGS=-DQL_ENABLE_LOGGING=0` 으로 만듭니다.

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

## 막힌 것

없음.

## 검증 기록

- 작업 단위 2 시점: `windows-clang` 113/113 통과, `QL_ENABLE_LOGGING=0` 별도
  build directory 에서도 113/113 통과. 두 구성의 `log.c.obj` 에서 zf_log
  심볼이 각각 19 개와 0 개로 확인됩니다.
- 작업 단위 3 시점: 두 구성 모두 114/114 통과. 비활성 레벨 호출 오버헤드는
  **0.071 ns/call** 로 측정되었고 연속 3 회 편차가 0.0005 ns 입니다. 측정
  방법과 기계 정보는 `docs/runtime-services/logging.md` 에 있습니다.

## 다음에 할 것

작업 단위 4. G2 예산.
