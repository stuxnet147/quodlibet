# W1 진행 기록

브랜치: `stuxnet147/w1-semantic-c-frontend`
지시서: `docs/workstreams/W1.md`

이 파일은 세션이 끊겼을 때 다음 세션이 읽는 유일한 기록입니다. 작업 단위를 커밋할 때 같이 커밋합니다.

## 지금 하는 것

IR concrete interpreter (`src/ir_interp.c`) 설계와 구현. W1 1단계의 두 번째 항목입니다.

## 기준선

- 시작 커밋 `6c6a34e`, 조율자의 커버리지 기준선 `cfbbb20` 위로 rebase 완료
- 시작 시점 `ctest --preset windows-clang`: 98/98 통과, 9.73초

### 조율자가 준 커버리지 기준선 (`docs/coverage/baseline-20260810.md`)

train 스플릿 서로 다른 C 본문 29,893 기준입니다.

| 단계 | 비율 |
|---|---:|
| 파싱 | 99.96% |
| 프런트엔드 수용 | 99.81% |
| IR 로어링 `SUPPORTED` | **0.18%** (53개) |

첫 차단 사유는 `unsupported_type` 53.86%, `unsupported_pointer` 43.91% 로 둘이 97.77% 입니다. **파서는 이미 G9 관문을 넘었고 남은 것은 전부 IR 로어링입니다.** 그 표는 첫 차단 사유이므로 타입과 포인터를 닫은 뒤 반드시 다시 재야 합니다. 빠른 반복은 val 1,050 본문입니다.

이 기준선이 2단계의 우선순위를 확정했습니다. W1.md 가 예상한 순서(정수 promotion 먼저)와 달리 **타입 폭과 포인터가 먼저**입니다. 측정이 순서를 이깁니다.

## 현재 코드 상태 조사 결과

### `src/ir.c` 가 이미 하는 검사

`graph_validate()` 가 builder `finish` 와 artifact `parse_graph` 양쪽에서 돌고, 다음을 이미 검사합니다.

- 타입표 정합성, pointer element 전방 참조 금지
- value definition kind 별 필드 규칙, 상수 바이트 폭과 canonical 표현
- opcode 별 operand/result arity 와 타입 규칙, terminator 종류별 필드 규칙
- 블록 명령 목록이 명령 전체를 정확히 분할하는지, PHI 가 블록 앞머리에 모여 있는지
- 도달 가능성, v1 acyclic 제약, 지배자 트리와 SSA 지배관계, PHI 의 incoming block 집합

### `src/ir.c` 가 하지 않던 검사 (verifier 가 새로 추가)

- **UB guard 의무**. 부분 연산(`SDIV` `UDIV` `SREM` `UREM` `SHL` `LSHR` `ASHR` `LOAD` `STORE`)에 데이터 의존하는 관찰이 그 사이를 가르는 `UB_GUARD` 없이 일어나는지
- effect 비트와 opcode 부류의 정확한 대응 (`ir.c` 는 `LOAD`/`STORE` 의 MEMORY, `CALL` 의 CALL 만 봄)
- extension opcode 거부

### 현재 로어링의 UB 처리 방식

`src/c_lower.c` 는 UB guard 를 연산 지점에 붙이지 않고 `lower_value.defined` (bool 값) 와 `may_ub` 플래그로 정의성 술어를 전파한 뒤, 관찰 지점에서 `emit_ub_guard()` 로 `UB_GUARD(defined)` 하나를 냅니다. guard 지점은 다섯 곳입니다.

| 위치 | 지점 |
|---|---|
| `c_lower.c:2098` | 대입 |
| `c_lower.c:2288` | 선언 초기화 |
| `c_lower.c:2417` | `if` 조건 |
| `c_lower.c:2540` | `return` |
| `c_lower.c:2586` | expression statement |

따라서 verifier 의 UB 규칙은 "연산 옆에 guard 가 있는가" 가 아니라 **"부분 연산에 데이터 의존하는 관찰 지점과 그 연산 사이를 지배하는 `UB_GUARD` 가 가르는가"** 입니다.

`&&` 와 `||` 는 `lower_logical_expression()` 이 값을 SELECT 로 eager 하게 계산하되 정의성만 `!rhs_needed || right_defined` 로 단락시킵니다. 즉 IR 은 RHS 를 계산하지만 그 정의성은 필요할 때만 요구합니다. interpreter 는 이 모델과 맞아야 하므로 부분 연산이 즉시 trap 하지 않고 **undefined 표시값(poison)** 을 내는 방식이어야 합니다.

## 설계 결정

### D1. verifier 는 builder 검증과 코드를 공유하지 않는다

`src/ir_verify.c` 는 공개 reader API (`ql_ir_get_view`, `ql_ir_block_at`, `ql_ir_instruction_at`, `ql_ir_value_at`, `ql_ir_type_at`) 만 써서 CFG, 지배자, 타입 규칙을 **독립적으로 다시 유도**합니다. `src/ir.c` 의 내부 함수를 재사용하면 한쪽 버그가 다른 쪽을 가려서 검증 가치가 사라집니다. 중복 구현은 의도한 비용입니다.

지배자도 알고리즘을 일부러 다르게 골랐습니다. `ir.c` 는 위상순서 LCA 로 immediate dominator 트리를 만들고, verifier 는 **지배자 집합 비트벡터를 위상순서로 한 번 교집합**해서 만듭니다. schema v1 이 acyclic 이므로 한 패스로 고정점입니다.

비트벡터 비용 때문에 블록 16384 개 상한이 있습니다. 넘으면 **명시적으로 거부**하고 부분 검증하지 않습니다(`QL_IR_VERIFY_MODULE`).

### D2. verifier 는 status 코드를 새로 만들지 않는다

`include/quodlibet/status.h` 는 W1 소유가 아닙니다. `ql_ir_verify()` 는 기존 status 로 매핑하고, 정확한 사유와 위치는 `ql_ir_verify_report_v1` 이 담습니다. 인자 오류(null, ABI)와 검증 실패를 구별하려면 report 의 `code` 를 봅니다. 인자 오류일 때는 `QL_IR_VERIFY_OK` 로 남습니다.

### D3. UB guard 의무는 배치만 검사하고 충분성은 interpreter 가 본다

verifier 는 **guard 의 존재와 배치**를 구조적으로 검사합니다. guard 술어가 실제로 충분한지(`b != 0` 를 빠뜨리지 않았는지)는 SMT 없이 구조 검사로 못 봅니다. 그 부분은 IR interpreter 가 구체 입력마다 담당합니다.

- verifier: 의무가 걸린 곳에 guard 가 있는가 (모든 입력에 대해, 구조적으로)
- interpreter: 그 guard 술어가 맞는가 (구체 입력마다, 실행으로)

### D4. guard 자신의 피연산자는 관찰이 아니다

정의성 술어는 부분 연산 결과를 정당하게 읽습니다. signed left shift 로어링은 넓은 타입 `SHL` 결과를 재확장과 비교해서 좁은 shift 가 정의되는지 판단합니다. guard 피연산자를 관찰로 세면 이 정당한 패턴이 거짓 위반이 됩니다.

### D5. PHI 는 의무의 방출 지점이다

join 블록의 guard 는 어느 분기도 지배하지 못하므로 PHI 로 들어오는 각 값의 의무는 **그 값이 온 선행 블록 끝까지** 해소되어 있어야 합니다. 현재 로어링이 분기 안 대입 지점에서 guard 를 내므로 이 규칙과 맞습니다.

## ir.h 확장 기록

W2 가 `include/quodlibet/ir.h` 를 읽고 있으므로 append-only 확장만 하고 여기에 한 줄씩 남깁니다.

- (아직 없음. `ir_verify.h` 는 새 헤더이고 `ir.h` 자체는 건드리지 않았습니다)

## 공유 파일 접촉

- `include/quodlibet/quodlibet.h` 에 `#include "quodlibet/ir_verify.h"` 한 줄 추가. 이 umbrella 헤더는 누구의 소유도 아니고 W2/W3 도 새 헤더를 넣을 것이므로 충돌 가능성을 여기 적어 둡니다.

## W2 가 볼 인터페이스

### `ql_ir_verify` (`include/quodlibet/ir_verify.h`)

```c
ql_status ql_ir_verify(const ql_allocator *allocator, const ql_ir *ir,
                       ql_ir_verify_report_v1 *report, ql_error *error);
```

`QL_STATUS_OK` 는 검증 통과입니다. 실패는 매핑된 status 와 `report.code`(`ql_ir_verify_code`), `report.block/instruction/value`, `report.message` 로 옵니다. **W2 는 product program 을 만들기 전에 좌우 IR 각각에 이것을 걸어도 됩니다.**

- (`ql_ir_interp_*` 가 나오면 여기에 적습니다)

## 완료한 작업 단위

### 1. IR verifier (`src/ir_verify.c`, `include/quodlibet/ir_verify.h`)

커밋: `9519c83`

- 공개 reader API 만으로 타입표, 값표, 참조, 블록 소유, terminator, CFG 도달성, v1 acyclic, 지배자, SSA 지배관계, PHI 선행 블록 대응, effect 규칙, UB guard 의무를 검사
- `tests/test_ir_verify.cpp` 8개 시험
- `tests/test_c_lower.cpp` 의 `ResultView()` 에 verifier 관문을 넣어 **로어링 시험 전부**가 자동으로 검증을 거치게 함
- `ctest` 106/106 통과 (기준선 98 + 8)

## 막힌 것

- 없음

## 조율자에게 요청할 것

커버리지 도구(`quodlibet coverage`)에 **`SUPPORTED` 인 본문에 대해 `ql_ir_verify` 를 돌리고 실패를 별도 열로 세는 계측**을 넣어 주시면 좋겠습니다. 지금은 53개뿐이라 시험 표본으로도 충분하지만, 타입과 포인터를 열면 수만 개가 되므로 코퍼스 전체에 대한 verifier 통과가 G8 종료 조건("IR verifier 가 로어링 출력 전부에 대해 통과한다")의 유일한 증거가 됩니다.

## 다음에 할 것

1. `src/ir_interp.c` 와 인터페이스 문서화 (W2 counterexample replay 가 그대로 씀)
2. clang 대조 differential 시험. UBSan 을 함께 써서 "IR 이 UB 라고 했는데 실제로는 정의된" 경우도 잡습니다
3. 퍼저 (파서, 로어링, IR 디코더)
4. 2단계 커버리지: **타입 폭 먼저, 그 다음 포인터**. 열고 나서 val 1,050 으로 재측정
