# W1 진행 기록

브랜치: `stuxnet147/w1-semantic-c-frontend`
지시서: `docs/workstreams/W1.md`

이 파일은 세션이 끊겼을 때 다음 세션이 읽는 유일한 기록입니다. 작업 단위를 커밋할 때 같이 커밋합니다.

## 지금 하는 것

2단계. 포인터를 닫았고 첫 차단 사유가 struct/union/enum 타입으로 옮겨갔습니다. 다음은 aggregate 타입입니다.

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

## 메모리 모델 계약 (W2 가 miter 에서 그대로 써야 하는 것)

조율자가 flat address space 로 확정했습니다(2026-08-10). `ARCHITECTURE.md` 의 profile 절에 전문이 있습니다. 여기에는 W2 가 바로 쓸 형태로 적습니다.

### 모델

`ASM2C_GNU_V1` 의 메모리는 **평평한 64비트 주소 공간**입니다. 포인터는 주소일 뿐이고 그 이상의 provenance 를 담지 않습니다. 이 프로파일의 원재료가 x86-64 SysV 목적 코드에서 복원한 C 라서 `(char *)p + n` 과 정수 왕복이 일상적이기 때문입니다.

저장 영역은 **object** 로 기술합니다. 각 object 는 base 주소와 크기를 갖고, 세 가지 제약이 항상 겁니다.

1. 서로 다른 살아있는 object 의 바이트 범위는 **disjoint** 합니다
2. 모든 object 는 **첫 페이지보다 위**에 있습니다 (`QL_IR_INTERP_FIRST_OBJECT_ADDRESS` = 0x1000). 주소 0 은 어떤 object 에도 속하지 않으므로 null 역참조가 항상 undefined 입니다
3. object 의 범위는 주소 공간을 **wrap 하지 않습니다**

`ql_ir_interp_run` 이 이 셋을 검사하고, 어기면 실행을 거부합니다. 모델이 인정하지 않는 배치에서 답을 내지 않습니다.

### 접근 정의 조건

주소 `a` 에서 `W` 바이트 접근이 정의되는 조건입니다.

```
defined(a, W)  :=  (어떤 살아있는 object O 에 대해
                      O.base <=u a  and  a - O.base <=u O.size
                      and  O.size - (a - O.base) >=u W)
                   and  a mod natural_alignment(W) == 0
```

`natural_alignment(W)` 는 W 가 16 이하의 2의 거듭제곱이면 W, 아니면 1 입니다. C 스칼라 배치가 아닌 폭에는 정렬을 요구하지 않습니다.

### 이 술어를 두 번 구현하지 않는 방법

조율자가 "ir_interp 와 miter 가 한 글자까지 같은 술어를 써야 한다"고 했습니다. 답은 **구현을 둘로 두지 않는 것**입니다.

- **증명에 쓰이는 술어는 IR 안에 있습니다.** 로어링이 object 의 base 와 size 를 IR 파라미터로 받아, 접근 지점마다 위 조건을 평범한 bit-vector 연산으로 계산하고 `UB_GUARD` 로 감쌉니다. interpreter 는 그냥 평범한 opcode 를 실행하고, W2 의 miter 도 그냥 평범한 opcode 를 인코딩합니다. **술어는 한 곳에만 있습니다.**
- interpreter 가 `LOAD`/`STORE` 의 부분성을 **독립으로** 한 번 더 압니다. 이것은 중복이 아니라 교차 검증입니다. guard 가 빠지거나 약하면 구체 입력에서 `GUARD_INSUFFICIENT` 로 잡힙니다. verifier 와 builder 를 분리한 것과 같은 이유입니다.

**W2 는 base/size 를 따로 만들어 낼 필요가 없습니다.** IR 이 이미 그것을 파라미터로 갖고 있고 guard 도 IR 안에 있으므로, miter 는 위 세 제약(disjoint, 첫 페이지 위, wrap 없음)만 좌우 공통 assumption 으로 걸면 됩니다. 그 형태는 로어링이 붙는 다음 작업 단위에서 확정해 여기 다시 적습니다.

### object 의 IR 파라미터 배치 (확정)

IR 파라미터 목록은 이 순서입니다.

```
[ C 파라미터, source 순서 ]
[ __memory,  포인터 파라미터가 하나라도 있을 때 한 개 ]
[ <이름>.__base 와 <이름>.__size, 포인터 파라미터마다, source 순서 ]
```

근거입니다.

- **앞의 N개가 C 인자와 그대로 대응**합니다. problem v2 의 인자 대응이 좌우 IR 파라미터 인덱스를 그냥 쓰면 됩니다
- object 표는 **포인터 파라미터에서 유도**되므로 좌우가 아무것도 주고받지 않아도 같은 표를 얻습니다. 같은 signature 면 같은 표입니다
- 포인터 파라미터가 없는 함수는 예전과 **완전히 같은 IR** 을 냅니다. 쓰지도 않을 memory 파라미터가 붙지 않습니다
- base/size 는 `bv64` 이고 `__memory` 는 유일한 MEMORY 타입 값이라 이름과 타입 둘 다로 찾을 수 있습니다

entry 블록이 모델의 세 제약을 `ASSUME` 으로 겁니다. object 마다 `FIRST <= base`, `size != 0`, `base <= base + size`(wrap 없음)이고, 서로 다른 object 쌍마다 `base_i + size_i <= base_j 또는 base_j + size_j <= base_i`(disjoint)입니다. **W2 는 이것을 다시 만들 필요가 없습니다. IR 안에 이미 있습니다.**

접근 guard 도 IR 안에 있습니다. 접근마다 `UB_GUARD(OR_i in_object_i(a, W) AND aligned(a, W))` 이고, `in_object_i` 는 `base_i <= a`, `a - base_i <= size_i`, `W <= size_i - (a - base_i)` 입니다. interpreter 의 `interp_access_defined` 와 같은 형태이고, miter 는 그냥 opcode 를 인코딩하면 됩니다.

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
- **메모리를 쓰는 모듈은 `options.objects` 로 object 표를 줍니다.** MEMORY 타입 파라미터는 바이트를 받지 않고 이름만으로 묶입니다(초기 이미지는 object 표가 줍니다). `object.final_image` 를 주면 실행이 끝난 메모리 이미지를 거기에 씁니다. differential 시험이 메모리 관찰을 비교할 때 쓰는 통로입니다
- 메모리 값은 SSA 라 **오래된 버전도 계속 읽힙니다.** store 뒤에 이전 memory 값을 대상으로 load 하는 IR 이 적법하므로, 버전을 write 사슬로 들고 있습니다
- undefined 값을 store 하면 결과 메모리 전체가 undefined 가 됩니다. 바이트 단위 추적보다 보수적인 쪽이고, 그것을 읽는 관찰이 여전히 guard 부족을 보고합니다

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

### 4. 타입 폭: void, typedef, cast

커밋: `4880e40`

조율자의 기준선이 `unsupported_type` 53.86% 와 `unsupported_pointer` 43.91% 를 지목했고, val 1,050 본문에서 `unsupported_type` 694건의 서명을 직접 뜯어보니 두 가지가 지배적이었습니다.

- 366건이 `void` 반환입니다. 로어링이 `void` 를 타입 어휘에 아예 갖고 있지 않았습니다
- 286건이 포인터도 구조체 태그도 없이 **typedef 이름만으로** 막혔습니다. 코퍼스는 익명화된 디컴파일러 C 라 `typedef int TYP_0;` 같은 사슬이 도처에 있습니다

#### `src/c_types.c` 와 `src/c_types.h`

스칼라 C 타입 모델을 로어링에서 떼어냈습니다. 철자 어휘, C11 6.3.1.1 정수 promotion, 6.3.1.8 usual arithmetic conversion, 표현 가능성 판정이 여기 있고 IR 을 전혀 모릅니다. 2,900줄짜리 `c_lower.c` 안에서 트리 순회와 뒤섞여 있던 규칙들을 따로 읽을 수 있게 하는 것이 목적입니다.

폭은 **타겟 ABI(x86_64 Linux LP64)** 기준이라 `long` 은 64비트입니다. 호스트가 아닙니다.

헤더가 `src/` 에 있는 비공개 헤더라 시험이 직접 링크하지 못합니다. 그래서 `tests/test_c_lower_types.cpp` 가 로어링의 관찰 가능한 동작으로 검사합니다. 어차피 계약은 "로어링이 무엇을 받고 무엇을 거부하는가" 쪽입니다.

#### void

반환 타입에서만 `void` 를 받습니다(C 가 불완전 타입을 허용하는 유일한 자리). 매개변수나 지역 변수의 `void` 는 타입 오류입니다. `return;` 과 본문 끝에서 떨어지는 암묵적 반환이 같은 terminator 를 씁니다.

#### typedef 해석

`type_definition` 노드를 훑어 이름 표를 만들고, 철자가 어휘에 없으면 표를 따라 해석합니다. 세 가지를 지켰습니다.

- **이 unit 이 실제로 선언한 이름만 해석합니다.** `scalar_t__` 처럼 선언 없이 쓰인 이름에 뜻을 붙이면 그것은 추측이고, 타입에 대한 틀린 추측은 함수에 대한 틀린 답입니다
- typedef 가 포인터/배열/함수를 가리키면 **`unsupported_pointer`** 로 보고합니다. `unsupported_type` 으로 뭉뚱그리면 커버리지 표가 진짜 장애물을 가립니다
- 사슬 길이를 64로 묶습니다. `typedef A B; typedef B A;` 는 tree-sitter 가 받아들이는 번역 단위이므로 순환하면 안 됩니다

#### cast expression 과 tree-sitter 의 모호성

`(T)(e)` 는 T 가 타입 이름이면 cast 이고 호출 가능한 것이면 call 인데, 문법만으로는 구별되지 않습니다. tree-sitter 는 call 쪽으로 해소합니다. 그래서 **callee 가 괄호에 싸인 식별자이고 그 이름을 이 unit 이 typedef 로 선언했으며 같은 이름의 객체가 가리지 않을 때** 로어링이 cast 로 되돌립니다. 이것을 안 하면 코퍼스의 cast 들이 `unsupported_call` 칸에 쌓여서 표가 거짓말을 합니다.

가시적인 객체가 같은 이름을 가리면 진짜 call 입니다. C 의 유효 범위 규칙 그대로이고 시험이 고정합니다.

#### 결과 (val 1,050 본문)

| 첫 차단 사유 | 이전 | 이후 |
|---|---:|---:|
| (로어링 성공) | 0 | 2 |
| `unsupported_type` | 694 | **103** |
| `unsupported_pointer` | 334 | **832** |
| `unsupported_call` | 4 | 55 |
| `undeclared_identifier` | 5 | 31 |
| `unsupported_control_flow` | 0 | 8 |
| `unsupported_expression` | 4 | 8 |
| `unsupported_loop` | 0 | 2 |
| `frontend_unsupported` | 9 | 9 |

`unsupported_type` 이 85% 줄었고 **첫 차단 사유의 79% 가 포인터**로 드러났습니다. 이것이 조율자가 말한 "둘을 닫고 다시 재라" 의 결과입니다. 타입은 닫혔고 남은 103건은 struct/union/enum 과 미선언 이름입니다.

`undeclared_identifier` 31건은 `GLB_0` 같은 전역 변수입니다. 정적 저장 기간이 다음 순위 후보입니다.

#### 정확성을 같이 끌고 갔다

수용이 늘 때마다 differential 사례도 같이 늘렸습니다. `cast_narrow`, `cast_unsigned`, `cast_byte`, `named`, `named_signed` 5개가 추가되어 25개 함수가 실제 컴파일 실행과 대조됩니다. `ctest` 167/167 통과입니다.

### 5. 인터프리터 메모리 모델과 퍼저 이전

커밋: `cd629a2`

#### 퍼저를 `tests/fuzz/` 로

조율자가 `tests/fuzz/fuzz_*.c` 자동 타깃 규칙과 `linux-sanitize`, `linux-fuzz` 프리셋을 넣어 주었습니다. 드라이버 셋(`fuzz_c_frontend.c`, `fuzz_c_lower.c`, `fuzz_ir_decoder.c`)을 만들되 **타깃 본체는 `tests/fuzz/fuzz_targets.h`** 에 두어 libFuzzer 실행 파일과 매 `ctest` 마다 도는 결정적 캠페인이 같은 코드를 씁니다. 둘이 갈라지면 무엇이 시험되었는지에 대해 서로 다른 말을 하게 됩니다.

층마다 드라이버를 나눈 것은 libFuzzer 가 타깃별 코퍼스를 갖게 하려는 것입니다. dispatch 바이트 하나를 다시 발견하게 만들 이유가 없습니다.

#### 메모리 모델

위 "메모리 모델 계약" 절이 전문입니다. 구현은 이렇습니다.

- object 표는 `ql_ir_interp_options_v1` 이 받습니다. 세 제약을 `ql_ir_interp_run` 이 검사하고 어기면 실행을 거부합니다
- 메모리 버전은 **write 사슬**입니다. object 초기 이미지 위에 (주소, 폭, 바이트) 기록이 쌓이고, load 는 바이트마다 가장 최근 기록을 찾습니다. 스냅샷 복사가 없고 오래된 버전이 그대로 읽힙니다
- `LOAD`, `STORE`, `PTR_ADD`, `PTR_TO_BV`, `BV_TO_PTR` 를 실행합니다
- `PTR_ADD` 의 offset 이 포인터 폭과 다르면 **`UNSUPPORTED`** 입니다. schema v1 은 offset 이 bit-vector 라고만 하므로, 여기서 확장 규칙을 지어내면 SMT 쪽과 의미가 갈라집니다

`tests/test_ir_interp_memory.cpp` 7개 시험이 초기 이미지 읽기, null 역참조, object 밖 접근과 걸치는 접근, 정렬, store 가시성과 오래된 버전 보존, 거부되는 배치, 모델 밖 구문을 고정합니다. guard 를 일부러 `UB_GUARD(true)` 로 약하게 두어서, null 역참조가 `GUARD_INSUFFICIENT` 로 잡히는 것을 시험이 보입니다.

`ctest` 224/224 통과입니다.

### 6. 포인터 로어링

커밋: `6995fa4`

`c_lower.c` 가 포인터를 냅니다. 파라미터 타입(`int *p`), 메모리 스레딩, `*p`, `p[i]`, `*(p + i)`, `*p = v`, `p[i] = v`, 포인터 산술과 비교, null 비교입니다. object 파라미터 배치는 위 절에 확정해 적었습니다.

#### guard 는 접근 *앞*에 선다

verifier 규칙을 하나 나눠야 했습니다. 기존 규칙은 "부분 연산과 그것을 관찰하는 지점 사이를 guard 가 가른다"였는데, 메모리 접근에는 그대로 쓸 수 없습니다. **역참조를 먼저 하고 나중에 검사할 수는 없기 때문입니다.**

그래서 부분 연산을 둘로 나눴습니다.

| 부류 | 정의성이 | guard 위치 |
|---|---|---|
| `SDIV` `UDIV` `SREM` `UREM` `SHL` `LSHR` `ASHR` | 피연산자에 대한 사후 사실 | 연산과 관찰 사이 |
| `LOAD` `STORE` | 주소에 대한 **사전 조건** | 접근 **앞** |

임의 구분이 아닙니다. 술어가 연산의 사전 조건이냐 결과에 대한 사실이냐의 차이이고, 각 경우에 의무가 실제로 해소되는 지점이 다릅니다. 메모리 접근은 앞의 guard 가 의무를 해소하므로 결과가 pending 을 물려주지 않습니다.

interpreter 는 여전히 `LOAD`/`STORE` 의 부분성을 독립으로 알고 있으므로, guard 가 약하면 구체 입력에서 잡힙니다.

#### 결과 (val 1,050 본문)

| 첫 차단 사유 | 타입 작업 후 | 포인터 작업 후 |
|---|---:|---:|
| (로어링 성공) | 2 | **5** |
| `unsupported_pointer` | 832 | **241** |
| `unsupported_type` | 103 | **638** |
| `unsupported_call` | 55 | 90 |
| `undeclared_identifier` | 31 | 40 |
| `unsupported_expression` | 8 | 11 |
| `unsupported_control_flow` | 8 | 10 |
| `unsupported_loop` | 2 | 4 |
| `frontend_unsupported` | 9 | 9 |

**포인터가 71% 줄었고 차단이 타입으로 되돌아갔습니다.** 되돌아간 것이 아니라 옮겨간 것입니다. `struct TYP_0 *ARG_0` 같은 서명이 이제 포인터 관문을 통과해서 **가리키는 대상의 타입**에서 걸립니다. 코퍼스의 지배적인 모양이 struct 포인터이므로 예상된 이동이고, 다음 순위는 struct/union/enum 입니다.

#### differential 이 포인터 함수를 잰다

`tests/test_c_lower_pointers.cpp` 의 `MatchesCompiledExecutionOnPointerFunctions` 가 6개 포인터 함수를 실제 컴파일 실행과 대조합니다. 사례마다 무작위 배열 내용 256회이고, **반환값뿐 아니라 최종 메모리 이미지도 비교**합니다. 메모리가 관찰 대상이므로 그것을 비교하지 않으면 store 가 맞는지 아무것도 말하지 않습니다.

IR 은 `QL_IR_INTERP_FIRST_OBJECT_ADDRESS` 의 상징적 object 를 보고 참조는 진짜 배열을 봅니다. 이 사례들 중 주소를 반환하는 것이 없으므로 비교를 건너는 것은 데이터뿐이라 건전합니다.

`ctest` 253/253 통과입니다.

#### 아직 아닌 것

- **`&x`**: 지역 변수의 주소를 잡으려면 이 slice 가 아직 만들지 않는 object 가 필요합니다. `unsupported_pointer` 로 남습니다
- 지역 포인터 변수 선언(`int *p = ...;`)은 declarator 경로가 아직 막습니다
- `p - q`(포인터 차이), 이중 포인터, typedef 가 가리키는 포인터
- `struct`/`union`/`enum` 과 `->`, `.`

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

1. **struct, union, enum.** 첫 차단 사유의 61% (638/1050) 입니다. 포인터를 열자 차단이 여기로 옮겨왔습니다. `->` 와 `.` 가 같이 갑니다
2. `&x` 와 지역 포인터 선언. 지역 object 를 만들면 둘 다 열립니다 연산, object identity, provenance, 유효 범위, alignment, load 와 store 입니다. **interpreter 에 메모리 모델을 같이 넣어야 합니다.** 지금 interpreter 는 포인터를 만나면 `UNSUPPORTED` 를 냅니다
2. 전역 변수와 정적 저장 기간 (`undeclared_identifier` 31건)
3. struct, union, enum (`unsupported_type` 잔여 103건)
4. 함수 호출과 외부 효과 (`unsupported_call` 55건)
5. `docs/lowering/adding-a-construct.md` 와 그 절차대로 추가한 구문 하나

각 단계마다 differential 사례를 같이 늘리고 val 1,050 으로 재측정합니다.
