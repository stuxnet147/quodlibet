# W1 진행 기록

브랜치: `stuxnet147/w1-semantic-c-frontend`
지시서: `docs/workstreams/W1.md`

이 파일은 세션이 끊겼을 때 다음 세션이 읽는 유일한 기록입니다. 작업 단위를 커밋할 때 같이 커밋합니다.

## 지금 하는 것

2단계. 타입 폭과 포인터, 그리고 cast expression. 조율자 지시로 1단계를 닫고 넘어왔습니다.

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

### `ql_ir_interp_run` (`include/quodlibet/ir_interp.h`)

**이것이 W2 의 counterexample replay 부품입니다.** SAT model 을 typed input 으로 decode 한 뒤 좌우 IR 에 각각 이것을 돌려서 관찰값을 비교하면 replay 가 됩니다.

```c
ql_status ql_ir_interp_run(const ql_allocator *allocator, const ql_ir *ir,
                           const ql_ir_interp_input_v1 *inputs,
                           size_t input_count,
                           const ql_ir_interp_options_v1 *options,
                           ql_ir_interp_result_v1 *result, ql_error *error);
```

계약입니다.

- 입력 바이트 인코딩은 **IR 상수와 완전히 같습니다.** 정확한 폭, little-endian, 마지막 바이트의 남는 상위 비트는 0, `_Bool` 은 1바이트 0 또는 1
- 모듈의 **모든 parameter 를 정확히 한 번씩** 묶어야 합니다. 빠지거나 겹치면 status 실패입니다
- status 실패는 "실행을 시작하지 못했다"이지 함수에 대한 판정이 아닙니다
- 결과 `outcome` 은 `RETURN` `TRAP` `TERMINATE` `UNDEFINED_BEHAVIOR` `DIVERGE` `ASSUMPTION_VIOLATED` `UNSUPPORTED` `STEP_LIMIT` 입니다
- **`UNSUPPORTED` 는 판정이 아닙니다.** 포인터, 메모리, 호출, event trace, 부동소수는 아직 모델이 없습니다. 어떤 proof method 도 이것을 함수에 대한 진술로 읽으면 안 됩니다
- `UNDEFINED_BEHAVIOR` 일 때 `ub_reason` 이 `GUARD_FAILED` `GUARD_UNDEFINED` `GUARD_INSUFFICIENT` `TERMINATOR` 중 하나를 줍니다
- 반환값은 `has_value`, `value_type`, `value[value_size]` 로 옵니다. 부호는 IR 이 갖고 있지 않으므로 호출자가 source signature 로 해석합니다
- bit-vector 폭 상한은 256 비트입니다. 64비트 signed 곱셈이 128비트 중간값을 쓰므로 여유를 둔 값입니다

undefined 전파 규칙은 로어링과 맞물려 있습니다.

- 부분 연산은 즉시 멈추지 않고 **undefined 표시값**을 냅니다. 순수 연산을 타고 전파됩니다
- `SELECT` 만 예외로 **조건과 선택된 쪽에서만** 전파합니다. 로어링이 `&&` 와 `||` 의 양쪽을 eager 하게 계산하고 정의성만 단락시키기 때문입니다. 선택되지 않은 쪽에서 전파하면 C 에 없는 UB 를 만들어 냅니다
- undefined 가 **관찰될 때만** 실행이 UB 가 됩니다. `UB_GUARD` 술어가 거짓이거나, 분기 조건이나 반환값이 undefined 이거나, UB terminator 입니다

## 완료한 작업 단위

### 1. IR verifier (`src/ir_verify.c`, `include/quodlibet/ir_verify.h`)

커밋: `9c07bb2`

- 공개 reader API 만으로 타입표, 값표, 참조, 블록 소유, terminator, CFG 도달성, v1 acyclic, 지배자, SSA 지배관계, PHI 선행 블록 대응, effect 규칙, UB guard 의무를 검사
- `tests/test_ir_verify.cpp` 8개 시험
- `tests/test_c_lower.cpp` 의 `ResultView()` 에 verifier 관문을 넣어 **로어링 시험 전부**가 자동으로 검증을 거치게 함
- `ctest` 106/106 통과 (기준선 98 + 8)

### 2. IR concrete interpreter 와 differential 시험

커밋: `82424ba`

- `src/ir_interp.c`, `include/quodlibet/ir_interp.h`. 320비트 내부 표현의 고정 용량 bit-vector 산술(add, sub, mul, udiv/sdiv/urem/srem, shl/lshr/ashr, 비교, zext/sext/trunc)을 직접 구현했습니다. `__int128` 같은 컴파일러 확장에 기대지 않습니다
- 나눗셈은 restoring shift-subtract 이고 곱셈은 32비트 limb schoolbook 이라 64비트 중간 타입만 씁니다
- `tests/test_ir_interp.cpp` 10개 시험
- `tests/test_ir_differential.cpp` 3개 시험, 20개 C 함수

#### differential 시험 설계

**참조는 C 의 모델이 아니라 같은 C 함수를 이 시험 바이너리를 만드는 그 컴파일러가 컴파일해서 실제로 돌린 것입니다.** `QL_DIFF_FUNCTION` 매크로가 함수 정의와 로어링에 넘길 소스 문자열을 **한 텍스트에서** 만들기 때문에 둘이 갈라질 수 없습니다.

이 방식이 "clang 이 없는 환경" 문제를 없앱니다. 이 시험 바이너리가 존재한다는 것이 참조가 컴파일되었다는 뜻이므로 비교는 항상 실행됩니다. 미실행 구간이 없습니다.

두 가지 제약이 따라옵니다.

- **타겟 ABI 는 x86_64 Linux LP64 라 `long` 이 64비트인데 Windows 는 32비트입니다.** 그래서 시험은 타겟과 두 호스트가 폭에 동의하는 타입만 씁니다. `char`, `short`, `int`, `unsigned`, `long long`. `long` 은 일부러 뺐습니다. 그것을 쓰면 서로 다른 두 함수를 비교하게 됩니다
- **참조를 UB 입력으로 부르지 않습니다.** UB 를 실제로 실행하면 비교 자체가 무의미해집니다. 각 사례가 어떤 입력이 정의되는지 말하고, overflow 정의성은 손으로 다시 쓴 규칙이 아니라 컴파일러의 `__builtin_*_overflow` 에서 옵니다

검사는 양쪽입니다.

- 정의된 입력: interpreter 가 `RETURN` 을 내고 값이 참조와 **정확히** 같아야 합니다
- 정의되지 않은 입력: interpreter 가 값이 아니라 `UNDEFINED_BEHAVIOR` 를 내야 합니다

`ExercisesBothSidesOfEveryPartialOperation` 이 부분 연산 사례마다 UB 입력에 실제로 닿았는지를 셉니다. **닿지 않으면 조용히 통과하지 않고 실패합니다.** 그렇게 하지 않으면 guard 를 하나도 검사하지 않고 통과하는 시험이 됩니다.

입력은 경계값 24개의 교차곱(사례당 576쌍)과 splitmix64 결정적 난수 2,000쌍입니다.

#### 이 과정에서 확인한 것

- 부호 있는 곱셈의 128비트 중간 경로가 맞습니다 (`3037000500^2` 가 UB 로 잡힙니다)
- `b != 0 && a / b > 1` 이 `b == 0` 에서 UB 가 아니라 0 을 냅니다. `SELECT` 전파 규칙이 없으면 여기서 거짓 UB 가 났을 것입니다
- 나눗셈, 나머지의 0 방향 절단과 나머지 부호가 clang 과 일치합니다

### 3. 퍼저

커밋: `948e7c3`

`tests/test_fuzz.cpp` 하나가 두 역할을 합니다. `QL_FUZZ_LIBFUZZER` 를 정의하고 컴파일하면 `LLVMFuzzerTestOneInput` 만 남고, 정의하지 않으면 같은 타깃을 결정적으로 도는 GoogleTest 캠페인이 됩니다. 그래서 지금 당장 매 `ctest` 마다 돌면서, 나중에 libFuzzer 실행 파일을 붙일 때 타깃을 다시 쓸 필요가 없습니다.

크래시 0 은 이 시험들이 가장 약하게 검사하는 성질입니다. 같이 거는 불변식이 본론입니다.

- 디코더가 받아들인 모듈은 verifier 를 **크래시 없이** 통과하거나 거부당해야 합니다
- **로어링이 `SUPPORTED` 라고 하면 그 IR 은 반드시 검증을 통과해야 합니다.** 변이된 소스도 소스이므로 G8 의 "verifier 가 로어링 출력 전부에 대해 통과한다"가 여기에 그대로 걸립니다
- 검증을 통과한 모듈은 interpreter 에서 크래시 없이 돌아야 합니다

크래시만 보는 퍼저는 로어링이 정당화할 수 없는 IR 을 내는 동안에도 초록으로 남습니다.

#### 캠페인이 헛돌지 않는지 세어 둔다

도달 횟수를 세서 로그에 찍고, 0 이면 시험이 실패합니다. 현재 수치입니다.

| 캠페인 | 반복 | 도달 |
|---|---:|---|
| 파서 (강한 변이) | 30,000 | 파싱 성공 2,708 |
| 로어링 (약한 변이 3/4) | 30,000 | `SUPPORTED` 417, 전부 검증 통과 |
| IR 디코더 (바이트 변이) | 60,000 | 디코드 1,038, 검증 통과 934 |
| 임의 바이트 | 30,000 | 파싱 성공 70 |

처음에는 로어링 도달이 4,000회 중 40회였습니다. 강한 변이가 파서 오류 복구에는 닿지만 로어링까지 살아남는 소스를 거의 남기지 않아서입니다. 약한 변이 모드를 넣어 417 로 올렸습니다. 두 모드는 서로 다른 타깃을 노립니다.

바이트 변이는 단일 비트 뒤집기 외에 **32비트 필드 통째 교체**를 넣었습니다. 개수와 오프셋이 little-endian 32비트라 비트 하나 뒤집기로는 표 범위 경계에 잘 닿지 않습니다.

#### 남은 것

`CMakeLists.txt` 와 `CMakePresets.json` 이 조율자 소유라 **libFuzzer 실행 파일과 Linux sanitize 프리셋은 아직 없습니다.** 아래 "조율자에게 요청할 것" 에 적었습니다.

## 막힌 것

- 없음

## 알게 된 로어링 공백 (2단계 후보)

시험을 쓰면서 발견한 것들입니다. 커버리지 재측정으로 순위를 정합니다.

- **cast expression** (`(short)(a + b)`) 이 `lower_expression` 에 없습니다
- conditional expression (`?:`), comma, `sizeof` 도 없습니다
- `long` 은 로어링이 64비트로 봅니다 (LP64 타겟). differential 시험에서는 호스트와 어긋나므로 제외했습니다

## 조율자에게 요청할 것

### 1. libFuzzer 타깃과 Linux sanitize 프리셋

`tests/test_fuzz.cpp` 를 `QL_FUZZ_LIBFUZZER` 를 정의하고 `-fsanitize=fuzzer,address,undefined` 로 컴파일하는 실행 파일 하나만 있으면 됩니다. `LLVMFuzzerTestOneInput` 이 이미 그 안에 있습니다. `CMakeLists.txt` 와 `CMakePresets.json` 이 조율자 소유라 W1 이 직접 못 합니다.

지금 상태로도 결정적 캠페인이 매 `ctest` 마다 돌지만, **coverage-guided 가 아니므로 G8 의 "퍼징 크래시 0" 을 완전히 닫았다고 보지 않습니다.**

### 2. 커버리지 도구의 verifier 계측

커버리지 도구(`quodlibet coverage`)에 **`SUPPORTED` 인 본문에 대해 `ql_ir_verify` 를 돌리고 실패를 별도 열로 세는 계측**을 넣어 주시면 좋겠습니다. 지금은 53개뿐이라 시험 표본으로도 충분하지만, 타입과 포인터를 열면 수만 개가 되므로 코퍼스 전체에 대한 verifier 통과가 G8 종료 조건("IR verifier 가 로어링 출력 전부에 대해 통과한다")의 유일한 증거가 됩니다.

## 다음에 할 것

1. 퍼저 (파서, 로어링, IR 디코더). Linux sanitize 구성에서 크래시 0
2. 2단계 커버리지: **타입 폭 먼저, 그 다음 포인터**. 열 때마다 differential 사례를 같이 늘리고 val 1,050 으로 재측정
3. `docs/lowering/adding-a-construct.md` 와 그 절차대로 추가한 구문 하나
